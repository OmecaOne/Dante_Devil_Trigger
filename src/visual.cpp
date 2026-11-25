// Visual-only transformation. Design and engine facts from Fitting Room 1.2.0 (maartenharms/fitting-room, GPL-3, BipedHooks.cpp,
// REAugments.cpp, BipedPost.cpp); IDs and offsets checked on 1.7.104.
//
// - The engine rebuilds an actor's equipment 3D in ID 24725 (equip change, load, cell change, race menu, UpdateEquipment). At +0x1EF it
//   calls ID 16096 with the worn-armor visitor (vtable ID 195851, loaded by a lea right before the call); that pass stages every worn
//   armor's armature into the biped. We run that pass unchanged with the engine's own visitor (Fitting Room: a proxy visitor crashed
//   Immersive Equipment Displays), then stage the costume with ID 17792 ApplyArmorAddon (last call per slot wins, so it also replaces
//   VisualWardrobe looks drawn during the pass). +0xFC calls 16096 too, with a different (checking) visitor: not hooked.
// - Every other armor slot (amulet, ring, cloak, mod slots) is hidden: NiAVObject kHidden on objects[slot].partClone, at once and
//   again from queued tasks (uncached models get their clone later). Skin parts stay (beast tails), except slot 52 (genitals under the
//   costume). Shields stay too (slot 39 or any armor flagged as a shield). Flags are cleared on the next pass of an actor we hid things on
//   (clones are reused across rebuilds).
// - Gameplay honesty: objects[slot].item of the costume slots is set back to the real worn armor (or the skin); .addon/.part keep drawing
//   the costume. Engine code (armor skill XP, equip conflicts) reads .item.
// - ID 24724 asks InventoryChanges::GetWornMask (ID 16044, call at +0x80) which head slots are covered to hide the FaceGen head and hair.
//   For a transformed actor we add the slots the costume really staged (measured on the biped, not its declared slots).
// - The 1st-person pass (player only) is told apart by the biped at visitor+0x18, read only behind the vtable check.
// - Head: with the head bit (slot 30) set, ID 24724 hides only the FaceGen node and skips the hair (disassembly: test mask,
//   1<<headObject -> or [faceNode+0xF4],1 -> return). In game the hair still showed through the costume, so the 3rd-person pass
//   also hides the face node and every head part node (every node whose name contains a head part's editor ID, extra parts included) itself,
//   inside the pass, again from tasks, and from a ~10 s watch after the pass that logs anything visible again. On untransform they are shown again inside the pass only: 24724 runs right after the pass (24725+0x2EB) and hides the
//   hair again if a real helmet asks for it; a queued show would undo that.

#include "visual.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Visual
{
	namespace
	{
		// ===== Forms =====

		constexpr std::string_view kPlugin = "OmecaOneDanteSDT.esp";
		constexpr RE::FormID       kCostumeIDs[2] = { 0x870, 0x876 };  // dsdtarmorA_wings_closed (DT), dsdtarmorB_wings_open (SDT)

		constexpr std::uint32_t kGenitalBit = 22;  // slot 52

		RE::TESObjectARMO* g_costumes[2]{};

		// ===== Engine functions =====

		// stages an armor's armature for the race into the biped (last call per slot wins)
		bool ApplyArmorAddon(RE::TESObjectARMO* a_armor, RE::TESRace* a_race, const RE::BSTSmartPointer<RE::BipedAnim>& a_holder, bool a_female)
		{
			using func_t = bool (*)(RE::TESObjectARMO*, RE::TESRace*, const void*, bool);
			static REL::Relocation<func_t> func{ REL::ID(17792) };
			return func(a_armor, a_race, &a_holder, a_female);
		}

		// SetEquipFlag + UpdateEquipment: make the engine rebuild the actor's equipment 3D (runs the worn pass)
		void SetEquipFlag(RE::AIProcess* a_process)
		{
			using func_t = void (*)(RE::AIProcess*, std::uint8_t);
			static REL::Relocation<func_t> func{ REL::ID(39907) };
			func(a_process, 1);
		}

		void UpdateEquipment(RE::AIProcess* a_process, RE::Actor* a_actor)
		{
			using func_t = void (*)(RE::AIProcess*, RE::Actor*);
			static REL::Relocation<func_t> func{ REL::ID(39395) };
			func(a_process, a_actor);
		}

		// ===== State =====
		// Written on the main thread, read in the passes (NPC 3D can load on other threads), so behind a mutex.

		struct ActorState
		{
			Costume       costume{ Costume::kNone };
			std::uint32_t staged{ 0 };     // 3rd person: slots the costume really staged (for the mask)
			std::uint32_t hidden[2]{};     // [3rd, 1st person]: slots we set kHidden on
			std::uint32_t generation[2]{};  // [3rd, 1st person], bumped on every pass; stale queued hides stop
			bool          headHidden{ false };  // we set kHidden on the face / head part nodes
		};

		std::mutex                                g_lock;
		std::unordered_map<RE::FormID, ActorState> g_states;

		// ===== Biped helpers =====

		// the actor's skin armor (NPC record, else race)
		RE::TESObjectARMO* SkinOf(RE::Actor* a_actor)
		{
			if (auto* base = a_actor->GetActorBase(); base && base->skin) {
				return base->skin;
			}
			auto* race = a_actor->GetRace();
			return race ? race->skin : nullptr;
		}

		// slots whose staged addon belongs to a_armor
		std::uint32_t StagedCoverage(RE::BipedAnim* a_biped, RE::TESObjectARMO* a_armor)
		{
			std::uint32_t mask = 0;
			for (std::uint32_t bit = 0; bit < 32; ++bit) {
				auto* addon = a_biped->objects[bit].addon;
				if (!addon) {
					continue;
				}
				for (auto* own : a_armor->armorAddons) {
					if (own == addon) {
						mask |= 1u << bit;
						break;
					}
				}
			}
			return mask;
		}

		// sets kHidden on the masked slots' clones; true while a masked slot has a model but no clone yet
		bool HideNodes(RE::BipedAnim* a_biped, std::uint32_t a_mask)
		{
			bool pending = false;
			for (std::uint32_t bit = 0; bit < 32; ++bit) {
				if (!(a_mask >> bit & 1u)) {
					continue;
				}
				auto& obj = a_biped->objects[bit];
				if (auto* node = obj.partClone.get()) {
					node->flags.set(RE::NiAVObject::Flag::kHidden);
				} else if (obj.part) {
					pending = true;
				}
			}
			return pending;
		}

		// clears kHidden on the masked slots' clones
		void ShowNodes(RE::BipedAnim* a_biped, std::uint32_t a_mask)
		{
			for (std::uint32_t bit = 0; bit < 32; ++bit) {
				if (a_mask >> bit & 1u) {
					if (auto* node = a_biped->objects[bit].partClone.get()) {
						node->flags.reset(RE::NiAVObject::Flag::kHidden);
					}
				}
			}
		}

		struct HeadResult
		{
			int         found{ 0 };    // nodes matched
			int         changed{ 0 };  // nodes whose flag had to change (hide: were visible)
			std::string names;         // the changed nodes' names (for the log)
		};

		// face node + all head parts (hair, brows, eyes, beard, ...) and their extra parts. Every node in the actor's 3D whose name
		// contains a head part's editor ID counts, not only the first exact match: physics mods (KS hair + SMP) can add renamed or
		// duplicate copies of a hair node (hair showed again after loading a save made while transformed).
		HeadResult SetHeadHidden(RE::Actor* a_actor, bool a_hide)
		{
			HeadResult r;
			auto*      root = a_actor->Get3D(false);
			auto*      base = a_actor->GetActorBase();
			if (!root || !base) {
				return r;
			}
			auto apply = [&](RE::NiAVObject* a_node) {
				if (!a_node) {
					return;
				}
				++r.found;
				if (a_node->flags.all(RE::NiAVObject::Flag::kHidden) == a_hide) {
					return;
				}
				++r.changed;
				if (r.names.size() < 200) {
					r.names += r.names.empty() ? "" : ", ";
					r.names += a_node->name.empty() ? "(unnamed)" : a_node->name.c_str();
				}
				if (a_hide) {
					a_node->flags.set(RE::NiAVObject::Flag::kHidden);
				} else {
					a_node->flags.reset(RE::NiAVObject::Flag::kHidden);
				}
			};
			std::vector<std::string_view> ids;
			for (int type = 0; type < static_cast<int>(RE::BGSHeadPart::HeadPartType::kTotal); ++type) {
				auto* part = base->GetCurrentHeadPartByType(static_cast<RE::BGSHeadPart::HeadPartType>(type));
				if (!part) {
					continue;
				}
				if (!part->formEditorID.empty()) {
					ids.emplace_back(part->formEditorID.c_str());
				}
				for (auto* extra : part->extraParts) {
					if (extra && !extra->formEditorID.empty()) {
						ids.emplace_back(extra->formEditorID.c_str());
					}
				}
			}
			auto* face = a_actor->GetFaceNodeSkinned();
			apply(face);
			RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* a_node) {
				if (a_node != face && !a_node->name.empty()) {
					const std::string_view name = a_node->name.c_str();
					// RaceMenu (SKEE) face overlays: copies of the face mesh under NPC Root, attached again after a load
					if (name.starts_with("Face [Ovl") || name.starts_with("Face [SOvl")) {
						apply(a_node);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					for (const auto id : ids) {
						if (name.find(id) != std::string_view::npos) {
							apply(a_node);
							break;
						}
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return r;
		}

		// S04 Slotless Armor (Nexus 192608) draws slotless rings/amulets outside the biped slots, each under a node "ARCH_<id>"
		// (e.g. 'SilverRingGarnet_1.nif' under 'ARCH_0000000000000016'). Hidden while transformed like the slotted jewelry; returns
		// how many nodes changed.
		int SetJewelryHidden(RE::Actor* a_actor, bool a_firstPerson, bool a_hide)
		{
			auto* root = a_actor->Get3D(a_firstPerson);
			if (!root) {
				return 0;
			}
			int changed = 0;
			RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* a_node) {
				if (!a_node->name.empty() && std::string_view(a_node->name.c_str()).starts_with("ARCH_")) {
					if (a_node->flags.all(RE::NiAVObject::Flag::kHidden) != a_hide) {
						++changed;
						if (a_hide) {
							a_node->flags.set(RE::NiAVObject::Flag::kHidden);
						} else {
							a_node->flags.reset(RE::NiAVObject::Flag::kHidden);
						}
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return changed;
		}

		// Re-hiding after a pass, in real time (a task queued from a task runs in the same frame, because SKSE drains the queue, so
		// the delays use a sleeping thread). Step 0 runs at once, then every 50 ms for 1 s, then every 250 ms up to 10 s after the pass:
		// RaceMenu re-attaches face overlays and other plugins (S04 smpRenotify) touch the 3D after a load. Anything found visible again
		// is re-hidden (head nodes are logged).
		constexpr int kFastSteps = 20;          // × 50 ms
		constexpr int kWatchSteps = 36;         // × 250 ms

		void QueueHide(RE::ActorHandle a_handle, bool a_firstPerson, std::uint32_t a_mask, std::uint32_t a_generation, int a_step = 0)
		{
			auto task = [=] {
				auto ptr = a_handle.get();
				auto* actor = ptr.get();
				if (!actor) {
					return;
				}
				{
					std::scoped_lock lock(g_lock);
					const auto it = g_states.find(actor->GetFormID());
					if (it == g_states.end() || it->second.costume == Costume::kNone || it->second.generation[a_firstPerson ? 1 : 0] != a_generation) {
						return;  // untransformed or rebuilt since
					}
				}
				if (auto* biped = actor->GetBiped1(a_firstPerson).get(); biped && a_mask) {
					HideNodes(biped, a_mask);
				}
				SetJewelryHidden(actor, a_firstPerson, true);
				if (!a_firstPerson) {
					const auto head = SetHeadHidden(actor, true);
					if (a_step > 0 && head.changed > 0) {
						const int ms = a_step <= kFastSteps ? a_step * 50 : kFastSteps * 50 + (a_step - kFastSteps) * 250;
						REX::INFO("visual: {:08X} head: {} node(s) visible again ~{} ms after the pass, re-hidden: {}", actor->GetFormID(),
							head.changed, ms, head.names);
					}
				}
				// 1st person runs the whole schedule too: S04 attaches its jewelry late
				if (a_step < kFastSteps + kWatchSteps) {
					QueueHide(a_handle, a_firstPerson, a_mask, a_generation, a_step + 1);
				}
			};
			if (a_step == 0) {
				SKSE::GetTaskInterface()->AddTask(std::move(task));
				return;
			}
			const int delay = a_step <= kFastSteps ? 50 : 250;
			std::thread([delay, task = std::move(task)]() mutable {
				std::this_thread::sleep_for(std::chrono::milliseconds(delay));
				SKSE::GetTaskInterface()->AddTask(std::move(task));
			}).detach();
		}

		// after an untransform pass: show the slots again from a task too (clones that arrive late)
		void QueueShow(RE::ActorHandle a_handle, bool a_firstPerson, std::uint32_t a_mask)
		{
			SKSE::GetTaskInterface()->AddTask([=] {
				auto ptr = a_handle.get();
				if (auto* actor = ptr.get()) {
					if (auto* biped = actor->GetBiped1(a_firstPerson).get()) {
						ShowNodes(biped, a_mask);
					}
				}
			});
		}

		// ===== Worn pass =====

		using VisitWorn_t = void (*)(RE::InventoryChanges*, RE::InventoryChanges::IItemChangeVisitor&);
		VisitWorn_t    g_origVisitWorn = nullptr;
		std::uintptr_t g_wornVisitorVtbl = 0;

		// the biped a worn pass fills (visitor+0x18), only for the engine's own visitor
		RE::BipedAnim* PassBiped(RE::InventoryChanges::IItemChangeVisitor& a_visitor)
		{
			const auto base = reinterpret_cast<std::uintptr_t>(&a_visitor);
			if (*reinterpret_cast<std::uintptr_t*>(base) != g_wornVisitorVtbl) {
				return nullptr;  // wrapped by another plugin: can't tell the passes apart
			}
			return *reinterpret_cast<RE::BipedAnim**>(base + 0x18);
		}

		// the real worn armor per slot, read before the pass (Fitting Room's SnapshotRealWorn)
		struct RealWorn
		{
			RE::TESObjectARMO* armor[32]{};
		};

		RealWorn SnapshotRealWorn(RE::InventoryChanges* a_changes)
		{
			RealWorn r;
			if (!a_changes->entryList) {
				return r;
			}
			for (auto* entry : *a_changes->entryList) {
				if (!entry || !entry->object || !entry->object->IsArmor() || !entry->IsWorn()) {
					continue;
				}
				auto* armor = entry->object->As<RE::TESObjectARMO>();
				const auto mask = armor ? std::to_underlying(armor->GetSlotMask()) : 0u;
				for (std::uint32_t bit = 0; bit < 32; ++bit) {
					if ((mask >> bit & 1u) && !r.armor[bit]) {
						r.armor[bit] = armor;
					}
				}
			}
			return r;
		}

		bool IsShield(RE::TESForm* a_item)
		{
			auto* armor = a_item ? a_item->As<RE::TESObjectARMO>() : nullptr;
			return armor && armor->IsShield();
		}

		// 0 = 3rd person, 1 = 1st person (player only), -1 = a pass we don't know
		int PassView(RE::Actor* a_actor, RE::InventoryChanges::IItemChangeVisitor& a_visitor)
		{
			auto* passBiped = PassBiped(a_visitor);
			if (!passBiped || passBiped == a_actor->GetBiped1(false).get()) {
				return 0;  // a wrapped visitor counts as the 3rd-person pass (Fitting Room does the same)
			}
			if (a_actor->IsPlayerRef() && passBiped == a_actor->GetBiped1(true).get()) {
				return 1;
			}
			return -1;
		}

		// after the engine's pass: stage the costume, report the real armor in its slots, hide the other slots, jewelry and head
		void Dress(RE::Actor* a_actor, bool firstPerson, Costume a_costume, const RealWorn& a_real, std::uint32_t a_prevHidden,
			std::uint32_t a_generation)
		{
			const auto& holder = a_actor->GetBiped1(firstPerson);
			auto*       biped = holder.get();
			auto*       race = a_actor->GetRace();
			auto*       base = a_actor->GetActorBase();
			auto*       costume = g_costumes[static_cast<int>(a_costume)];
			if (!biped || !race || !base || !costume) {
				return;
			}
			const int view = firstPerson ? 1 : 0;
			if (a_prevHidden) {
				ShowNodes(biped, a_prevHidden);
			}

			const bool ok = ApplyArmorAddon(costume, race, holder, base->IsFemale());
			const auto staged = StagedCoverage(biped, costume);
			std::uint32_t hide = 0;
			if (staged) {
				auto* skin = SkinOf(a_actor);
				for (std::uint32_t bit = 0; bit < 32; ++bit) {
					auto& obj = biped->objects[bit];
					if (staged >> bit & 1u) {
						// honesty: the costume slots report the real armor (or the skin) to engine code that reads .item
						auto* want = a_real.armor[bit] ? a_real.armor[bit] : skin;
						if (want && obj.item != want) {
							obj.item = want;
						}
						continue;
					}
					if (!obj.item && !obj.part) {
						continue;
					}
					if (obj.item == skin && skin && bit != kGenitalBit) {
						continue;  // skin parts (tails) stay
					}
					if (bit == RE::BIPED_OBJECTS::kShield || IsShield(obj.item)) {
						continue;  // shields stay visible
					}
					hide |= 1u << bit;
				}
				if (hide) {
					HideNodes(biped, hide);
				}
				SetJewelryHidden(a_actor, firstPerson, true);
				int headNodes = -1;
				if (!firstPerson) {
					headNodes = SetHeadHidden(a_actor, true).found;
				}
				// again from timed tasks (QueueHide): every 50 ms for 1 s, then every 250 ms up to 10 s
				QueueHide(a_actor->GetHandle(), firstPerson, hide, a_generation);
				if (!firstPerson) {
					REX::INFO("visual: {:08X} head nodes hidden: {}", a_actor->GetFormID(), headNodes);
				}
			}
			{
				std::scoped_lock lock(g_lock);
				auto& state = g_states[a_actor->GetFormID()];
				state.hidden[view] = hide;
				if (!firstPerson) {
					state.staged = staged;
					state.headHidden = staged != 0;
				}
			}
			REX::INFO("visual: {:08X} '{}' {} pass: {} {} staged 0x{:08X} (ApplyArmorAddon {}), hidden 0x{:08X}", a_actor->GetFormID(),
				a_actor->GetName(), firstPerson ? "1st" : "3rd", a_costume == Costume::kDT ? "DT" : "SDT", costume->GetName(), staged, ok, hide);
			if (!staged) {
				REX::WARN("visual: the costume has no armature for race {:08X} '{}'; actor left as it is", race->GetFormID(), race->GetName());
			}
		}

		// logs a pass of a known actor that took longer than 3 ms (to find hitches)
		struct PassTimer
		{
			std::chrono::steady_clock::time_point start{ std::chrono::steady_clock::now() };
			RE::Actor*                            actor{ nullptr };
			int                                   view{ -1 };
			const char*                           what{ "" };
			~PassTimer()
			{
				const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				if (actor && ms > 3.0) {
					REX::INFO("visual: {:08X} {} pass ({}) took {:.1f} ms", actor->GetFormID(), view == 1 ? "1st" : "3rd", what, ms);
				}
			}
		};

		// hook at 24725+0x1EF: the engine's pass, then the costume (transformed) or the undo of our hiding (just untransformed)
		void WornPass(RE::InventoryChanges* a_changes, RE::InventoryChanges::IItemChangeVisitor& a_visitor) noexcept
		{
			PassTimer timer;
			bool      passRan = false;
			try {
				auto* owner = a_changes ? a_changes->owner : nullptr;
				auto* actor = owner ? owner->As<RE::Actor>() : nullptr;
				Costume       costume = Costume::kNone;
				std::uint32_t prevHidden = 0;
				std::uint32_t generation = 0;
				bool          known = false;
				bool          headHidden = false;
				const int     view = actor ? PassView(actor, a_visitor) : -1;
				if (view >= 0) {
					std::scoped_lock lock(g_lock);
					if (const auto it = g_states.find(actor->GetFormID()); it != g_states.end()) {
						known = true;
						costume = it->second.costume;
						prevHidden = it->second.hidden[view];
						generation = ++it->second.generation[view];
						headHidden = it->second.headHidden;
					}
				}
				if (!known) {
					g_origVisitWorn(a_changes, a_visitor);
					return;
				}
				timer.actor = actor;
				timer.view = view;
				timer.what = costume == Costume::kNone ? "untransform" : "costume";
				const auto real = costume != Costume::kNone ? SnapshotRealWorn(a_changes) : RealWorn{};
				g_origVisitWorn(a_changes, a_visitor);
				passRan = true;

				if (costume != Costume::kNone) {
					Dress(actor, view == 1, costume, real, prevHidden, generation);
					return;
				}
				// untransformed: bring back what we hid in this view (reused clones keep the flag), then forget the actor
				if (prevHidden) {
					if (auto* biped = actor->GetBiped1(view == 1).get()) {
						ShowNodes(biped, prevHidden);
					}
					QueueShow(actor->GetHandle(), view == 1, prevHidden);
				}
				if (view == 0 && headHidden) {
					SetHeadHidden(actor, false);  // 24724 runs next and hides the hair again if a real helmet asks for it
				}
				SetJewelryHidden(actor, view == 1, false);
				std::scoped_lock lock(g_lock);
				if (const auto it = g_states.find(actor->GetFormID()); it != g_states.end() && it->second.costume == Costume::kNone) {
					it->second.hidden[view] = 0;
					if (view == 0) {
						it->second.staged = 0;
						it->second.headHidden = false;
					}
					if (!it->second.hidden[0] && !it->second.hidden[1] && !it->second.headHidden) {
						g_states.erase(it);
					}
				}
			} catch (...) {
				if (!passRan) {
					try {
						g_origVisitWorn(a_changes, a_visitor);
					} catch (...) {
					}
				}
			}
		}

		// ===== Worn mask =====

		using GetWornMask_t = std::uint32_t (*)(RE::InventoryChanges*);
		GetWornMask_t g_origGetWornMask = nullptr;

		// hook at 24724+0x80: a transformed actor's worn mask also has the slots the costume staged (hides FaceGen head / hair)
		std::uint32_t WornMask(RE::InventoryChanges* a_changes) noexcept
		{
			const auto real = g_origGetWornMask(a_changes);
			try {
				auto* owner = a_changes ? a_changes->owner : nullptr;
				if (!owner || !owner->IsActor()) {
					return real;
				}
				std::scoped_lock lock(g_lock);
				const auto it = g_states.find(owner->GetFormID());
				if (it != g_states.end() && it->second.costume != Costume::kNone) {
					const auto mask = real | it->second.staged;  // staged 0 (no pass yet / unsupported race): vanilla answer
					static RE::FormID    lastActor = 0;
					static std::uint32_t lastMask = 0;
					if (lastActor != owner->GetFormID() || lastMask != mask) {
						lastActor = owner->GetFormID();
						lastMask = mask;
						REX::INFO("visual: {:08X} worn mask {:08X} -> {:08X}", lastActor, real, mask);
					}
					return mask;
				}
			} catch (...) {
			}
			return real;
		}

		// ===== Install checks =====

		// destination of the E8 call at a_site
		std::uintptr_t CallTarget(std::uintptr_t a_site)
		{
			return a_site + 5 + *reinterpret_cast<const std::int32_t*>(a_site + 1);
		}

		bool InExe(std::uintptr_t a_addr)
		{
			const auto base = static_cast<std::uintptr_t>(REX::FModule::GetExecutingModule().GetBaseAddress());
			return a_addr >= base && a_addr < base + 0x4000000u;
		}

		// a rip-relative lea (48/4C 8D modrm with mod 00, rm 101) in the 0x80 bytes before the call that loads a_target
		bool LeaBefore(std::uintptr_t a_site, std::uintptr_t a_target)
		{
			for (std::uintptr_t p = a_site - 0x80; p + 7 <= a_site; ++p) {
				const auto* b = reinterpret_cast<const std::uint8_t*>(p);
				if ((b[0] == 0x48 || b[0] == 0x4C) && b[1] == 0x8D && (b[2] & 0xC7) == 0x05) {
					if (p + 7 + *reinterpret_cast<const std::int32_t*>(p + 3) == a_target) {
						return true;
					}
				}
			}
			return false;
		}

		// the hook site is a call to the expected function (or to another plugin's hook, which we chain)
		bool CheckCallSite(const char* a_name, std::uintptr_t a_site, std::uint64_t a_calleeID)
		{
			const REL::ID a_callee{ a_calleeID };
			if (*reinterpret_cast<const std::uint8_t*>(a_site) != 0xE8) {
				REX::ERROR("visual: {}: no call at the hook site (byte {:02X}); visual override disabled", a_name,
					*reinterpret_cast<const std::uint8_t*>(a_site));
				return false;
			}
			const auto target = CallTarget(a_site);
			if (target == a_callee.address()) {
				return true;
			}
			if (InExe(target)) {
				const std::uint64_t rva = target - static_cast<std::uintptr_t>(REX::FModule::GetExecutingModule().GetBaseAddress());
				REX::ERROR("visual: {}: the call goes to exe rva {:X}, not ID {}; visual override disabled", a_name, rva, a_calleeID);
				return false;
			}
			REX::WARN("visual: {}: already hooked by another plugin (target {:X}); chaining", a_name, target);
			return true;
		}

		// another plugin that replaces the head part update wouldn't ask our worn mask
		void ReportHeadFunction()
		{
			const auto  entry = REL::ID(24724).address();
			const auto* b = reinterpret_cast<const std::uint8_t*>(entry);
			if (b[0] == 0xE9 || (b[0] == 0xFF && b[1] == 0x25)) {
				REX::WARN("visual: ID 24724 (head part update) starts with a jump ({:02X} {:02X}): another plugin replaced it", b[0], b[1]);
			} else {
				REX::INFO("visual: ID 24724 entry unpatched ({:02X} {:02X} {:02X} {:02X})", b[0], b[1], b[2], b[3]);
			}
		}
	}

	// ===== Public =====

	bool Install()
	{
		const REL::Relocation<std::uintptr_t> passSite{ REL::ID(24725), 0x1EF };
		const REL::Relocation<std::uintptr_t> maskSite{ REL::ID(24724), 0x80 };
		g_wornVisitorVtbl = REL::ID(195851).address();
		if (!CheckCallSite("worn pass (24725+0x1EF)", passSite.address(), 16096) ||
			!CheckCallSite("worn mask (24724+0x80)", maskSite.address(), 16044)) {
			return false;
		}
		if (!LeaBefore(passSite.address(), g_wornVisitorVtbl)) {
			REX::ERROR("visual: the worn visitor (vtable ID 195851) isn't loaded before 24725+0x1EF; visual override disabled");
			return false;
		}
		auto& trampoline = REL::GetTrampoline();
		g_origVisitWorn = reinterpret_cast<VisitWorn_t>(trampoline.write_call<5>(passSite.address(), WornPass));
		g_origGetWornMask = reinterpret_cast<GetWornMask_t>(trampoline.write_call<5>(maskSite.address(), WornMask));
		REX::INFO("visual: hooks installed (worn pass 24725+0x1EF, worn mask 24724+0x80)");
		return true;
	}

	void OnDataLoaded()
	{
		ReportHeadFunction();
		for (int i = 0; i < 2; ++i) {
			g_costumes[i] = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESObjectARMO>(kCostumeIDs[i], kPlugin);
			if (!g_costumes[i]) {
				REX::ERROR("visual: costume armor {:03X} not found in {}", kCostumeIDs[i], kPlugin);
			}
		}
	}

	void SetCostume(RE::Actor* a_actor, Costume a_costume)
	{
		if (!a_actor) {
			return;
		}
		{
			std::scoped_lock lock(g_lock);
			const auto it = g_states.find(a_actor->GetFormID());
			const auto current = it == g_states.end() ? Costume::kNone : it->second.costume;
			if (current == a_costume) {
				return;
			}
			if (it == g_states.end()) {
				g_states[a_actor->GetFormID()].costume = a_costume;
			} else {
				it->second.costume = a_costume;  // kept until the next pass has shown the hidden slots again
			}
		}
		REX::INFO("visual: {:08X} '{}' costume -> {}", a_actor->GetFormID(), a_actor->GetName(),
			a_costume == Costume::kDT ? "DT" : a_costume == Costume::kSDT ? "SDT" : "none");
		auto* process = a_actor->currentProcess;
		if (!process || !process->middleHigh || !a_actor->Get3D(false)) {
			return;  // not loaded: the next load runs the pass
		}
		SetEquipFlag(process);
		UpdateEquipment(process, a_actor);
	}

	void Reset()
	{
		std::scoped_lock lock(g_lock);
		g_states.clear();
	}
}

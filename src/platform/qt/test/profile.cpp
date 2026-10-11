/* Copyright (c) 2026 Doomhack
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// Before mGBA's headers, whose snprintf macro for MSVC breaks QtTest's
#include <QTest>

#include "platform/qt/ProfileCompare.h"
#include "platform/qt/ProfileData.h"
#include "platform/qt/ProfileDemangler.h"
#include "platform/qt/ProfileModels.h"

#include <cstring>

#include <mgba-util/vfs.h>

using namespace QGBA;

// Three functions. main calls a clone and leaf; the clone calls leaf too. Function IDs: main 0,
// the clone 1, leaf 2, then the pseudo-functions, so [main] is 20 and [IRQ] 21.
static const char PROFILE[] =
	"mprof 1\n"
	"meta elapsed-cycles 100\n"
	"meta rom-crc32 12345678\n"
	"func 08000100 64 1 -1 main\n"
	"func 08000140 32 1 -1 _Z5cloneii.constprop.0\n"
	"func 08000160 16 1 -1 leaf\n"
	"count 08000100 10 2\n"
	"count 08000102 6 1\n"
	"count 08000140 30 3\n"
	"count 08000160 40 4\n"
	"count 08000162 14 2\n"
	"node -1 20 0 0 0 0\n"
	"node -1 21 0 0 0 0\n"
	"node 0 0 1 0 16 3\n"
	"node 2 1 2 0 30 3\n"
	"node 2 2 3 0 20 3\n"
	"node 3 2 1 0 34 3\n"
	"frame 280896 80896\n"
	"end\n";

class ProfileTest : public QObject {
Q_OBJECT

private:
	std::shared_ptr<ProfileData> load(const QByteArray& text = PROFILE) {
		VFile* vf = VFileFromConstMemory(text.constData(), text.size());
		std::shared_ptr<ProfileData> data = ProfileData::load(vf, "test");
		vf->close(vf);
		return data;
	}

private slots:
	void shortNames() {
		QCOMPARE(shortFunctionName("main"), QString("main"));
		QCOMPARE(shortFunctionName("big(int, int)"), QString("big"));
		QCOMPARE(shortFunctionName("big(int, int) [constprop.0]"), QString("big [constprop.0]"));
		QCOMPARE(shortFunctionName("void bn::sprite_ptr::set_position<int>(bn::fixed_t<12> const&) const"),
		         QString("bn::sprite_ptr::set_position"));
		QCOMPARE(shortFunctionName("bn::vector<int, 4>::operator[](int)"), QString("bn::vector::operator[]"));
		QCOMPARE(shortFunctionName("bool operator<(A const&, A const&)"), QString("operator<"));
		QCOMPARE(shortFunctionName("Foo::operator()(int)"), QString("Foo::operator()"));
		QCOMPARE(shortFunctionName("void* operator new(unsigned int)"), QString("operator new"));
		QCOMPARE(shortFunctionName("(anonymous namespace)::helper(int)"), QString("(anonymous namespace)::helper"));
		QCOMPARE(shortFunctionName("[DMA 3]"), QString("[DMA 3]"));
	}

	void data() {
		std::shared_ptr<ProfileData> data = load();
		QVERIFY(data);
		QCOMPARE(data->totalCycles(), 100ULL);
		QCOMPARE(data->name(1), QString("_Z5cloneii [constprop.0]"));
		QCOMPARE(data->functionByRawName("_Z5cloneii.constprop.0"), 1U);
		QCOMPARE(data->functionByRawName("missing"), mPROFILE_NONE);
		QVERIFY(data->isThumb(0));

		// [IRQ] has no time, so it isn't a root worth showing
		QCOMPARE(data->roots().size(), 1);
		// main's children, the most expensive first: the clone (with leaf below it), then leaf
		QCOMPARE(data->children(2), (QVector<uint32_t>{ 3, 4 }));
		// leaf's nodes, the most expensive first
		QCOMPARE(data->nodesOf(2), (QVector<uint32_t>{ 5, 4 }));
		QCOMPARE(data->frames().count, size_t(1));
	}

	void treeModel() {
		std::shared_ptr<ProfileData> data = load();
		ProfileTreeModel model;
		model.setProfile(data);
		QCOMPARE(model.rowCount(), 1);
		QModelIndex root = model.index(0, 0);
		QCOMPARE(root.data().toString(), QString("[main]"));
		QModelIndex main = model.index(0, 0, root);
		QCOMPARE(main.data().toString(), QString("main"));
		QCOMPARE(model.rowCount(main), 2);
		QModelIndex clone = model.index(0, ProfileTreeModel::TOTAL_CYCLES, main);
		QCOMPARE(clone.data(ProfileRole::SORT).toULongLong(), 64ULL);
		QCOMPARE(model.index(0, ProfileTreeModel::TOTAL_PERCENT, main).data().toString(), QString("64.00%"));
		QCOMPARE(model.parent(clone), main.sibling(main.row(), 0));
		QModelIndex leaf = model.indexOfNode(5);
		QCOMPARE(leaf.data(ProfileRole::FUNCTION).toUInt(), 2U);
		QCOMPARE(model.parent(leaf).data(ProfileRole::NODE).toUInt(), 3U);
	}

	void functionModel() {
		std::shared_ptr<ProfileData> data = load();
		ProfileFunctionModel model;
		model.setProfile(data);
		// The three functions, and [main] and [IRQ] don't count: they never ran or were called
		QCOMPARE(model.rowCount(), 3);
		int leaf = model.rowOf(2);
		QCOMPARE(model.index(leaf, ProfileFunctionModel::SELF_CYCLES).data(ProfileRole::SORT).toULongLong(), 54ULL);
		QCOMPARE(model.index(leaf, ProfileFunctionModel::CALLS).data().toString(), QString("4"));
		QVERIFY(model.index(model.rowOf(1), 0).data(ProfileRole::SEARCH).toString().contains("constprop"));

		ProfileEdgeModel callers(true);
		callers.setFunction(data, 2);
		QCOMPARE(callers.rowCount(), 2);
		// The clone's call costs more, so it comes first
		QCOMPARE(callers.index(0, ProfileEdgeModel::NAME).data(ProfileRole::FUNCTION).toUInt(), 1U);
		QCOMPARE(callers.index(0, ProfileEdgeModel::INCLUSIVE_CYCLES).data(ProfileRole::SORT).toULongLong(), 34ULL);
	}

	void instructionModel() {
		std::shared_ptr<ProfileData> data = load();
		ProfileInstructionModel model;
		// movs r0, #1; bx lr; then zeroes
		auto reader = [](uint32_t address, unsigned width, uint32_t* value) {
			*value = address == 0x08000100 ? 0x2001 : address == 0x08000102 ? 0x4770 : 0;
			return width == 2;
		};
		model.setFunction(data, 0, reader);
		// Every halfword of the 64-byte function
		QCOMPARE(model.rowCount(), 32);
		QCOMPARE(model.index(0, ProfileInstructionModel::ADDRESS).data().toString(), QString("08000100"));
		QCOMPARE(model.index(0, ProfileInstructionModel::CYCLES).data().toString(), QString("10"));
		QCOMPARE(model.index(0, ProfileInstructionModel::PERCENT).data().toString(), QString("62.50%"));
		QVERIFY(model.index(0, ProfileInstructionModel::INSTRUCTION).data().toString().startsWith("mov"));
		QVERIFY(model.index(1, ProfileInstructionModel::INSTRUCTION).data().toString().startsWith("bx"));
		// Instructions that never ran have no costs
		QCOMPARE(model.index(2, ProfileInstructionModel::CYCLES).data().toString(), QString());

		// Without a way to read code there are still costs, but no disassembly
		model.setFunction(data, 0, {});
		QCOMPARE(model.index(0, ProfileInstructionModel::INSTRUCTION).data().toString(), QString());
		QCOMPARE(model.index(1, ProfileInstructionModel::CYCLES).data().toString(), QString("6"));
	}

	void demangler() {
		char out[4096];
		auto demangle = [&out](const char* name) {
			size_t length = demangleItanium(name, strlen(name), out, sizeof(out));
			return length ? QString::fromUtf8(out, length) : QString();
		};
		QCOMPARE(demangle("_Z21I_PollWServEvents_e32v"), QString("I_PollWServEvents_e32()"));
		QCOMPARE(demangle("_Z28_txnal_runtime_error_get_msgPv"), QString("_txnal_runtime_error_get_msg(void*)"));
		QCOMPARE(demangle("_ZNKSt10bad_typeid4whatEv"), QString("std::bad_typeid::what() const"));
		QCOMPARE(demangle("_ZL9save_type"), QString("save_type"));
		// MSVC 19.51 miscompiled these with inlining on, as "big(int, (int))" (see llvm-demangle's README)
		QCOMPARE(demangle("_Z3bigii"), QString("big(int, int)"));
		QCOMPARE(demangle("_ZN3bar3bazEii"), QString("bar::baz(int, int)"));

		// Names from devkitARM's libstdc++, without clone suffixes, as the profiler passes them. Any
		// readable result will do: the text needn't match c++filt's.
		const char* names[] = {
			"_ZGVN9__gnu_cxx16bitmap_allocatorIcE13_S_mem_blocksE",
			"_ZGVNSt10moneypunctIcLb0EE2idE",
			"_ZL15eh_globals_dtorPv",
			"_ZL23__gxx_exception_cleanup19_Unwind_Reason_CodeP21_Unwind_Control_Block",
			"_ZN10__cxxabiv112_GLOBAL__N_1L12compute_sizeEjjj",
			"_ZN12_GLOBAL__N_110fast_float15powers_templateIvE17power_of_five_128E",
			"_ZN12_GLOBAL__N_110fast_floatL14parse_mantissaERNS0_6bigintERNS0_20parsed_number_stringEjRj",
			"_ZN12_GLOBAL__N_110fast_floatL8long_mulILt125EEEbRNS0_8stackvecIXT_EEENS0_4spanImEE",
			"_ZNKSi6sentrycvbEv",
			"_ZNSt13random_device16_M_getval_pretr1Ev",
			"_ZZ19__cxa_thread_atexitE4once",
		};
		for (const char* name : names) {
			QString demangled = demangle(name);
			QVERIFY2(!demangled.isEmpty() && !demangled.startsWith("_Z"), name);
		}
		// GCC's transactional memory clones are a GCC extension LLVM doesn't know. These may fall
		// back to the mangled name, but mustn't come out garbled.
		const char* extensions[] = {
			"_ZGTtNSt11logic_errorC1EPKc",
			"_ZGTtNSt11logic_errorD2Ev",
		};
		for (const char* name : extensions) {
			QString demangled = demangle(name);
			QVERIFY2(demangled.isEmpty() || !demangled.startsWith("_Z"), name);
		}

		// Short names from real demangler output
		QCOMPARE(shortFunctionName(demangle("_ZNKSt6ranges11__rotate_fnclIN9__gnu_cxx17__normal_iteratorIPNSt6chrono12_GLOBAL__N_"
		                                    "14RuleESt6vectorIS6_SaIS6_EEEESB_EENS_8subrangeIT_SD_Xqu18sized_sentinel_forISD_SD_"
		                                    "ELNS_13subrange_kindE1ELSE_0EEEESD_SD_T0_")),
		         QString("std::ranges::__rotate_fn::operator()"));
		QCOMPARE(shortFunctionName(demangle("_ZN12_GLOBAL__N_110fast_floatL8long_mulILt125EEEbRNS0_8stackvecIXT_EEENS0_4spanImEE")),
		         QString("(anonymous namespace)::fast_float::long_mul"));

		// Names that aren't mangled, or aren't valid, are left for the caller to show as they are
		QCOMPARE(demangleItanium("main", 4, out, sizeof(out)), size_t(0));
		QCOMPARE(demangleItanium("_Zfoo", 5, out, sizeof(out)), size_t(0));
		// Only the length given is read, so the clone suffix needn't be removed first
		const char* clone = "_ZNKSt10bad_typeid4whatEv.part.0";
		size_t length = demangleItanium(clone, strlen("_ZNKSt10bad_typeid4whatEv"), out, sizeof(out));
		QCOMPARE(QString::fromUtf8(out, length), QString("std::bad_typeid::what() const"));
		// Too long for the buffer: cut short
		char small[8];
		QCOMPARE(demangleItanium("_ZNKSt10bad_typeid4whatEv", 25, small, sizeof(small)), size_t(7));
		QCOMPARE(QString(small), QString("std::ba"));

		// Profiles show demangled names once the demangler is registered
		mProfileSetDemangler(demangleItanium);
		std::shared_ptr<ProfileData> data = load();
		mProfileSetDemangler(nullptr);
		QCOMPARE(data->name(1), QString("clone(int, int) [constprop.0]"));
		QCOMPARE(data->shortName(1), QString("clone [constprop.0]"));
		QCOMPARE(data->rawName(1), QString("_Z5cloneii.constprop.0"));
	}

	void comparison() {
		std::shared_ptr<ProfileData> baseline = load();
		// leaf takes 60 cycles more when main calls it directly
		QByteArray text(PROFILE);
		text.replace("count 08000160 40 4", "count 08000160 100 4");
		text.replace("node 2 2 3 0 20 3", "node 2 2 3 0 80 3");
		std::shared_ptr<ProfileData> current = load(text);
		auto comparison = std::make_shared<ProfileComparison>(baseline, current);
		// Neither has frames, so costs are totals
		QVERIFY(!comparison->perFrame());
		QCOMPARE(comparison->functions().size(), 3);

		ProfileCompareFunctionModel functions;
		functions.setComparison(comparison);
		int leaf = -1;
		for (int row = 0; row < functions.rowCount(); ++row) {
			if (functions.index(row, 0).data().toString() == "leaf") {
				leaf = row;
			}
		}
		QVERIFY(leaf >= 0);
		QCOMPARE(functions.index(leaf, ProfileCompareFunctionModel::SELF_BASELINE).data().toString(), QString("54"));
		QCOMPARE(functions.index(leaf, ProfileCompareFunctionModel::SELF_CURRENT).data().toString(), QString("114"));
		QCOMPARE(functions.index(leaf, ProfileCompareFunctionModel::SELF_CHANGE).data().toString(), QString("+111.1%"));
		QCOMPARE(functions.index(leaf, ProfileCompareFunctionModel::SELF_CHANGE).data(ProfileRole::SORT).toDouble(), 60.0);

		// Matched by call path: [main] > main > leaf is 20 cycles, then 80
		ProfileCompareTreeModel tree;
		tree.setComparison(comparison);
		QCOMPARE(tree.rowCount(), 1);
		QModelIndex main = tree.index(0, 0, tree.index(0, 0));
		QCOMPARE(main.data().toString(), QString("main"));
		QCOMPARE(tree.rowCount(main), 2);
		// leaf now costs more than the clone, so it comes first
		QModelIndex leafNode = tree.index(0, 0, main);
		QCOMPARE(leafNode.data().toString(), QString("leaf"));
		QCOMPARE(tree.index(0, ProfileCompareTreeModel::TOTAL_BASELINE, main).data().toString(), QString("20"));
		QCOMPARE(tree.index(0, ProfileCompareTreeModel::TOTAL_CHANGE, main).data().toString(), QString("+300.0%"));
		QCOMPARE(tree.parent(leafNode), main);
	}
};

QTEST_APPLESS_MAIN(ProfileTest)
#include "profile.moc"

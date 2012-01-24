/**
 * @file
 *
 * Test code for @ref bucket.cpp.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>
#include <boost/bind.hpp>
#include <boost/smart_ptr/scoped_array.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/foreach.hpp>
#include <boost/ref.hpp>
#include <string>
#include <vector>
#include <iterator>
#include <algorithm>
#include <utility>
#include <limits>
#include "testmain.h"
#include "../src/bucket.h"
#include "../src/bucket_internal.h"

using namespace std;
using namespace Bucket;
using namespace Bucket::internal;

/**
 * Tests for Range.
 */
class TestRange : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestRange);
    CPPUNIT_TEST(testConstructor);
    CPPUNIT_TEST(testAppendEmpty);
    CPPUNIT_TEST(testAppendOverflow);
    CPPUNIT_TEST(testAppendMiddle);
    CPPUNIT_TEST(testAppendEnd);
    CPPUNIT_TEST(testAppendGap);
    CPPUNIT_TEST(testAppendNewScan);
    CPPUNIT_TEST_SUITE_END();

public:
    void testConstructor();          ///< Test the constructors
    void testAppendEmpty();          ///< Appending to an empty range
    void testAppendOverflow();       ///< An append that would overflow the size
    void testAppendMiddle();         ///< Append to the middle of an existing range
    void testAppendEnd();            ///< Extend a range
    void testAppendGap();            ///< Append outside (and not adjacent) to an existing range
    void testAppendNewScan();        ///< Append with a different scan
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestRange, TestSet::perBuild());

void TestRange::testConstructor()
{
    Range empty;
    Range single(3, 6);
    Range range(2, UINT64_C(0xFFFFFFFFFFFFFFF0), 0x10);

    CPPUNIT_ASSERT_EQUAL(Range::size_type(0), empty.size);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), single.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(6), single.start);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), single.size);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(2), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0xFFFFFFFFFFFFFFF0)), range.start);

    CPPUNIT_ASSERT_THROW(Range(2, UINT64_C(0xFFFFFFFFFFFFFFF0), 0x11), std::out_of_range);
}

void TestRange::testAppendEmpty()
{
    Range range;
    bool success;

    success = range.append(3, 6);
    CPPUNIT_ASSERT_EQUAL(true, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(6), range.start);
}

void TestRange::testAppendOverflow()
{
    Range range;
    range.scan = 3;
    range.start = 0x90000000U;
    range.size = 0xFFFFFFFFU;
    bool success = range.append(3, range.start + range.size);
    CPPUNIT_ASSERT_EQUAL(false, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0xFFFFFFFFU), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0x90000000U), range.start);
}

void TestRange::testAppendMiddle()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(4, UINT64_C(0x12345678FFFF));
    CPPUNIT_ASSERT_EQUAL(true, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10000), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}

void TestRange::testAppendEnd()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(4, range.start + range.size);
    CPPUNIT_ASSERT_EQUAL(true, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10001), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}

void TestRange::testAppendGap()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(4, range.start + range.size + 1);
    CPPUNIT_ASSERT_EQUAL(false, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10000), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}

void TestRange::testAppendNewScan()
{
    Range range;
    range.scan = 4;
    range.start = UINT64_C(0x123456781234);
    range.size = 0x10000;
    bool success = range.append(5, range.start + range.size);
    CPPUNIT_ASSERT_EQUAL(false, success);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x10000), range.size);
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), range.scan);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), range.start);
}


/**
 * Tests for @ref Bucket::internal::RangeCounter.
 */
class TestRangeCounter : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestRangeCounter);
    CPPUNIT_TEST(testEmpty);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST_SUITE_END();
public:
    void testEmpty();           ///< Tests initial state
    void testSimple();          ///< Tests state after various types of additions
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestRangeCounter, TestSet::perBuild());

void TestRangeCounter::testEmpty()
{
    RangeCounter c;
    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(0), c.countRanges());
    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(0), c.countSplats());
}

void TestRangeCounter::testSimple()
{
    RangeCounter c;

    c.append(3, 5);
    c.append(3, 6);
    c.append(3, 6);
    c.append(4, 7);
    c.append(5, 2);
    c.append(5, 4);
    c.append(5, 5);
    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(4), c.countRanges());
    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(7), c.countSplats());
}

/**
 * Tests for @ref Bucket::internal::RangeCollector.
 */
class TestRangeCollector : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestRangeCollector);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testFlush);
    CPPUNIT_TEST(testFlushEmpty);
    CPPUNIT_TEST_SUITE_END();

public:
    void testSimple();            ///< Test basic functionality
    void testFlush();             ///< Test flushing and continuing
    void testFlushEmpty();        ///< Test flushing when nothing to flush
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestRangeCollector, TestSet::perBuild());

void TestRangeCollector::testSimple()
{
    vector<Range> out;

    {
        RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));
        c.append(3, 5);
        c.append(3, 6);
        c.append(3, 6);
        c.append(4, UINT64_C(0x123456781234));
        c.append(5, 2);
        c.append(5, 4);
        c.append(5, 5);
    }
    CPPUNIT_ASSERT_EQUAL(4, int(out.size()));

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(5), out[0].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), out[1].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[1].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(UINT64_C(0x123456781234)), out[1].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(5), out[2].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[2].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(2), out[2].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(5), out[3].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[3].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(4), out[3].start);
}

void TestRangeCollector::testFlush()
{
    vector<Range> out;
    RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));

    c.append(3, 5);
    c.append(3, 6);
    c.flush();

    CPPUNIT_ASSERT_EQUAL(1, int(out.size()));
    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(5), out[0].start);

    c.append(3, 7);
    c.append(4, 0);
    c.flush();

    CPPUNIT_ASSERT_EQUAL(3, int(out.size()));

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(2), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(5), out[0].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(3), out[1].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[1].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(7), out[1].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(4), out[2].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(1), out[2].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0), out[2].start);
}

void TestRangeCollector::testFlushEmpty()
{
    vector<Range> out;
    RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));
    c.flush();
    CPPUNIT_ASSERT_EQUAL(0, int(out.size()));
}


/**
 * Slow tests for Bucket::internal::RangeCounter and Bucket::internal::RangeCollector.
 * These tests are designed to catch overflow conditions and hence necessarily
 * involve running O(2^32) operations. They are thus nightly rather than
 * per-build tests.
 */
class TestRangeBig : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestRangeBig);
    CPPUNIT_TEST(testBigRange);
    CPPUNIT_TEST(testManyRanges);
    CPPUNIT_TEST_SUITE_END();
public:
    void testBigRange();             ///< Throw more than 2^32 contiguous elements into a range
    void testManyRanges();           ///< Create more than 2^32 separate ranges
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestRangeBig, TestSet::perNightly());

void TestRangeBig::testBigRange()
{
    vector<Range> out;
    RangeCollector<back_insert_iterator<vector<Range> > > c(back_inserter(out));
    RangeCounter counter;

    for (std::tr1::uint64_t i = 0; i < UINT64_C(0x123456789); i++)
    {
        c.append(0, i);
        counter.append(0, i);
    }
    c.flush();

    CPPUNIT_ASSERT_EQUAL(2, int(out.size()));
    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(2), counter.countRanges());
    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(UINT64_C(0x123456789)), counter.countSplats());

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(0), out[0].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0xFFFFFFFFu), out[0].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0), out[0].start);

    CPPUNIT_ASSERT_EQUAL(Range::scan_type(0), out[1].scan);
    CPPUNIT_ASSERT_EQUAL(Range::size_type(0x2345678Au), out[1].size);
    CPPUNIT_ASSERT_EQUAL(Range::index_type(0xFFFFFFFFu), out[1].start);
}

void TestRangeBig::testManyRanges()
{
    RangeCounter counter;

    // We force each append to be a separate range by going up in steps of 2.
    for (std::tr1::uint64_t i = 0; i < UINT64_C(0x123456789); i++)
    {
        counter.append(0, i * 2);
    }

    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(UINT64_C(0x123456789)), counter.countRanges());
    CPPUNIT_ASSERT_EQUAL(std::tr1::uint64_t(UINT64_C(0x123456789)), counter.countSplats());
}

std::ostream &operator<<(std::ostream &o, const Cell &cell)
{
    return o << "Cell("
        << cell.getBase()[0] << ", "
        << cell.getBase()[1] << ", "
        << cell.getBase()[2] << ", " << cell.getLevel() << ")";
}

/**
 * Test code for @ref Bucket::internal::forEachCell.
 */
class TestForEachCell : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestForEachCell);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testAsserts);
    CPPUNIT_TEST_SUITE_END();

private:
    vector<Cell> cells;
    bool cellFunc(const Cell &cell);

public:
    void testSimple();          ///< Test normal usage
    void testAsserts();         ///< Test the assertions of preconditions
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestForEachCell, TestSet::perBuild());

bool TestForEachCell::cellFunc(const Cell &cell)
{
    cells.push_back(cell);

    Cell::size_type lower[3], upper[3];
    cell.getCorners(lower, upper);
    return (lower[0] <= 2 && 2 < upper[0]
        && lower[1] <= 1 && 1 < upper[1]
        && lower[2] <= 4 && 4 < upper[2]);
}

void TestForEachCell::testSimple()
{
    const Cell::size_type dims[3] = {4, 4, 6};
    forEachCell(dims, 4, boost::bind(&TestForEachCell::cellFunc, this, _1));
    /* Note: the recursion order of forEachCell is not defined, so this
     * test is constraining the implementation. It should be changed
     * if necessary.
     */
    CPPUNIT_ASSERT_EQUAL(15, int(cells.size()));
    CPPUNIT_ASSERT_EQUAL(Cell(0, 0, 0, 3), cells[0]);
    CPPUNIT_ASSERT_EQUAL(Cell(0, 0, 0, 2), cells[1]);
    CPPUNIT_ASSERT_EQUAL(Cell(0, 0, 4, 2), cells[2]);
    CPPUNIT_ASSERT_EQUAL(Cell(0, 0, 4, 1), cells[3]);
    CPPUNIT_ASSERT_EQUAL(Cell(2, 0, 4, 1), cells[4]);
    CPPUNIT_ASSERT_EQUAL(Cell(2, 0, 4, 0), cells[5]);
    CPPUNIT_ASSERT_EQUAL(Cell(3, 0, 4, 0), cells[6]);
    CPPUNIT_ASSERT_EQUAL(Cell(2, 1, 4, 0), cells[7]);
    CPPUNIT_ASSERT_EQUAL(Cell(3, 1, 4, 0), cells[8]);
    CPPUNIT_ASSERT_EQUAL(Cell(2, 0, 5, 0), cells[9]);
    CPPUNIT_ASSERT_EQUAL(Cell(3, 0, 5, 0), cells[10]);
    CPPUNIT_ASSERT_EQUAL(Cell(2, 1, 5, 0), cells[11]);
    CPPUNIT_ASSERT_EQUAL(Cell(3, 1, 5, 0), cells[12]);
    CPPUNIT_ASSERT_EQUAL(Cell(0, 2, 4, 1), cells[13]);
    CPPUNIT_ASSERT_EQUAL(Cell(2, 2, 4, 1), cells[14]);
}

// Not expected to ever be called - just to give a legal function pointer
static bool dummyCellFunc(const Cell &cell)
{
    (void) cell;
    return false;
}

void TestForEachCell::testAsserts()
{
    const Cell::size_type dims[3] = {4, 4, 6};
    CPPUNIT_ASSERT_THROW(forEachCell(dims, 100, dummyCellFunc), std::invalid_argument);
    CPPUNIT_ASSERT_THROW(forEachCell(dims, 0, dummyCellFunc), std::invalid_argument);
    CPPUNIT_ASSERT_THROW(forEachCell(dims, 3, dummyCellFunc), std::invalid_argument);
}

/**
 * Test code for @ref Bucket::internal::forEachSplat.
 */
class TestForEachSplat : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestForEachSplat);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testEmpty);
    CPPUNIT_TEST_SUITE_END();
private:
    typedef pair<Range::scan_type, Range::index_type> Id;
    vector<vector<char> > fileData;
    boost::ptr_vector<FastPly::Reader> files;

    void splatFunc(Range::scan_type scan, Range::index_type id, const Splat &splat, vector<Id> &out);
public:
    virtual void setUp();

    void testSimple();
    void testEmpty();
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestForEachSplat, TestSet::perBuild());

void TestForEachSplat::splatFunc(Range::scan_type scan, Range::index_type id, const Splat &splat, vector<Id> &out)
{
    // Check that the ID information we're given matches what we encoded into the splats
    CPPUNIT_ASSERT_EQUAL(scan, Range::scan_type(splat.position[0]));
    CPPUNIT_ASSERT_EQUAL(id, Range::index_type(splat.position[1]));

    out.push_back(Id(scan, id));
}

void TestForEachSplat::setUp()
{
    CppUnit::TestFixture::setUp();
    int size = 100000;
    int nFiles = 5;
    string header =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 100000\n"
        "property float32 x\n"
        "property float32 y\n"
        "property float32 z\n"
        "property float32 nx\n"
        "property float32 ny\n"
        "property float32 nz\n"
        "property float32 radius\n"
        "end_header\n";

    fileData.resize(nFiles);
    for (int i = 0; i < nFiles; i++)
    {
        boost::scoped_array<float> values(new float[size * 7]);
        fill(values.get(), values.get() + size * 7, 0.0f);
        for (int j = 0; j < size; j++)
        {
            values[j * 7 + 0] = i;
            values[j * 7 + 1] = j;
        }

        copy(header.begin(), header.end(), back_inserter(fileData[i]));
        copy(reinterpret_cast<const char *>(values.get()),
             reinterpret_cast<const char *>(values.get() + size * 7),
             back_inserter(fileData[i]));
        files.push_back(new FastPly::Reader(&fileData[i][0], fileData[i].size(), 2.0f));
    }
}

void TestForEachSplat::testSimple()
{
    vector<Id> expected, actual;
    vector<Range> ranges;

    ranges.push_back(Range(0, 0));
    ranges.push_back(Range(0, 2, 3));
    ranges.push_back(Range(1, 2, 3));
    ranges.push_back(Range(2, 100, 40000)); // Large range to test buffering

    BOOST_FOREACH(const Range &range, ranges)
    {
        for (Range::index_type i = 0; i < range.size; ++i)
        {
            expected.push_back(Id(range.scan, range.start + i));
        }
    }

    forEachSplat(files, ranges.begin(), ranges.end(),
                 boost::bind(&TestForEachSplat::splatFunc, this, _1, _2, _3, boost::ref(actual)));
    CPPUNIT_ASSERT_EQUAL(expected.size(), actual.size());
    for (size_t i = 0; i < actual.size(); i++)
    {
        CPPUNIT_ASSERT_EQUAL(expected[i].first, actual[i].first);
        CPPUNIT_ASSERT_EQUAL(expected[i].second, actual[i].second);
    }
}

void TestForEachSplat::testEmpty()
{
    vector<Range> ranges;
    vector<Id> actual;

    forEachSplat(files, ranges.begin(), ranges.end(),
                 boost::bind(&TestForEachSplat::splatFunc, this, _1, _2, _3, boost::ref(actual)));
    CPPUNIT_ASSERT(actual.empty());
}

/// Test for @ref Bucket::splatCellIntersect.
class TestSplatCellIntersect : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestSplatCellIntersect);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST_SUITE_END();
public:
    void testSimple();         ///< Test normal use cases
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestSplatCellIntersect, TestSet::perBuild());

void TestSplatCellIntersect::testSimple()
{
    Splat splat;
    splat.position[0] = 10.0f;
    splat.position[1] = 20.0f;
    splat.position[2] = 30.0f;
    splat.radius = 3.0f;

    // Only the lower grid extent matters. The lower corner of the
    // grid is at -8.0f, -2.0f, 2.0f with spacing 2.0f.
    const float ref[3] = {-10.0f, -10.0f, -10.0f};
    Grid grid(ref, 2.0f, 1, 100, 4, 100, 6, 100);

    // Cell covers (0,10,20)-(8,18,28) in world space
    CPPUNIT_ASSERT(splatCellIntersect(splat, Cell(4, 6, 9, 2), grid));
    // Cell covers (0,10,20)-(4,14,24) in world space
    CPPUNIT_ASSERT(!splatCellIntersect(splat, Cell(4, 6, 9, 1), grid));
    // Cell covers (10,20,30)-(12,22,32) (entirely inside bounding box)
    CPPUNIT_ASSERT(splatCellIntersect(splat, Cell(9, 11, 14, 0), grid));
}

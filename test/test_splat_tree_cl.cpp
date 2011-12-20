/**
 * @file
 *
 * Test code for @ref SplatTreeCL.
 */

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>
#include <climits>
#include <cstddef>
#include <vector>
#include <cmath>
#include <limits>
#include "testmain.h"
#include "test_clh.h"
#include "test_splat_tree.h"
#include "../src/splat_tree_cl.h"

using namespace std;

class TestSplatTreeCL : public TestSplatTree, public CLH::Test::Mixin
{
    CPPUNIT_TEST_SUB_SUITE(TestSplatTreeCL, TestSplatTree);
    CPPUNIT_TEST(testLevelShift);
    CPPUNIT_TEST(testPointBoxDist2);
    CPPUNIT_TEST(testMakeCode);
    CPPUNIT_TEST(testSolveQuadratic);
    // TODO CPPUNIT_TEST(testSphereFit);
    // TODO CPPUNIT_TEST(testProjectDist);
    CPPUNIT_TEST_SUITE_END();

protected:
    virtual void build(
        std::size_t &numLevels,
        std::vector<SplatTree::command_type> &commands,
        std::vector<SplatTree::command_type> &start,
        const std::vector<Splat> &splats, const Grid &grid);

private:
    cl::Program octreeProgram, mlsProgram;

    /**
     * Variant of @c CppUnit::assertDoublesEqual that accepts relative or absolute error.
     *
     * - If @a expected or @a actual is NaN, fails.
     * - If @a expected or @a actual is an infinity, fails unless they're equal.
     * - Otherwise, passes if |@a expected - @a actual| <= @a eps or
     *   if |@a expected - @a actual| <= @a eps * @a expected.
     *
     * @param expected     Expected value
     * @param actual       Actual value
     * @param eps          Error tolerance
     * @param sourceLine   Pass @c CPPUNIT_SOURCELINE
     *
     * @see @ref ASSERT_DOUBLES_EQUAL
     */
    static void assertDoublesRelEqual(double expected, double actual, double eps, const CppUnit::SourceLine &sourceLine);

    int callLevelShift(cl_int ilox, cl_int iloy, cl_int iloz, cl_int ihix, cl_int ihiy, cl_int ihiz);
    float callPointBoxDist2(float px, float py, float pz, float lx, float ly, float lz, float hx, float hy, float hz);
    int callMakeCode(cl_int x, cl_int y, cl_int z);
    float callSolveQuadratic(float a, float b, float c);

    void testLevelShift();     ///< Test @ref levelShift in @ref octree.cl.
    void testPointBoxDist2();  ///< Test @ref pointBoxDist2 in @ref octree.cl.
    void testMakeCode();       ///< Test @ref makeCode in @ref octree.cl.
    void testSolveQuadratic(); ///< Test @ref solveQuadratic in @ref mls.cl.
public:
    virtual void setUp();
    virtual void tearDown();
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestSplatTreeCL, TestSet::perBuild());

/**
 * Macro wrapper around @ref TestSplatTreeCL::assertDoublesRelEqual.
 */
#define ASSERT_DOUBLES_EQUAL(actual, expected, eps) \
    TestSplatTreeCL::assertDoublesRelEqual(actual, expected, eps, CPPUNIT_SOURCELINE())

void TestSplatTreeCL::assertDoublesRelEqual(double expected, double actual, double eps, const CppUnit::SourceLine &sourceLine)
{
    string expectedStr = boost::lexical_cast<string>(expected);
    string actualStr = boost::lexical_cast<string>(actual);
    if ((tr1::isnan)(expected) && (tr1::isnan)(actual))
        return;
    if ((tr1::isnan)(expected))
        CppUnit::Asserter::failNotEqual(expectedStr, actualStr, sourceLine, "expected is NaN");
    if ((tr1::isnan)(actual))
        CppUnit::Asserter::failNotEqual(expectedStr, actualStr, sourceLine, "actual is NaN");
    if (expected == actual)
        return;
    if ((tr1::isfinite)(expected) && (tr1::isfinite)(actual))
    {
        double err = abs(expected - actual);
        if (err > eps * abs(expected) && err > eps)
            CppUnit::Asserter::failNotEqual(expectedStr, actualStr, sourceLine,
                                            "Delta   : " + boost::lexical_cast<string>(err));
    }
    else
        CppUnit::Asserter::failNotEqual(expectedStr, actualStr, sourceLine, "");
}

void TestSplatTreeCL::setUp()
{
    TestSplatTree::setUp();
    setUpCL();
    map<string, string> defines;
    defines["UNIT_TESTS"] = "1";
    defines["WGS_X"] = "4";  // arbitrary values to make it compile
    defines["WGS_Y"] = "4";
    defines["WGS_Z"] = "4";
    octreeProgram = CLH::build(context, "kernels/octree.cl", defines);
    mlsProgram = CLH::build(context, "kernels/mls.cl", defines);
}

void TestSplatTreeCL::tearDown()
{
    tearDownCL();
    TestSplatTree::tearDown();
}

void TestSplatTreeCL::build(
    std::size_t &numLevels,
    std::vector<SplatTree::command_type> &commands,
    std::vector<SplatTree::command_type> &start,
    const std::vector<Splat> &splats, const Grid &grid)
{
    SplatTreeCL tree(context, 9, 1001);
    std::vector<cl::Event> events(1);
    tree.enqueueBuild(queue, &splats[0], splats.size(), grid, 0, CL_FALSE, NULL, NULL, &events[0]);

    std::size_t commandsSize = tree.getCommands().getInfo<CL_MEM_SIZE>();
    std::size_t startSize = tree.getStart().getInfo<CL_MEM_SIZE>();
    commands.resize(commandsSize / sizeof(SplatTree::command_type));
    start.resize(startSize / sizeof(SplatTree::command_type));
    queue.enqueueReadBuffer(tree.getCommands(), CL_TRUE, 0, commandsSize, &commands[0]);
    queue.enqueueReadBuffer(tree.getStart(), CL_TRUE, 0, startSize, &start[0]);
    numLevels = tree.getNumLevels();
}

int TestSplatTreeCL::callLevelShift(cl_int ilox, cl_int iloy, cl_int iloz, cl_int ihix, cl_int ihiy, cl_int ihiz)
{
    cl_int ans;
    cl::Buffer out(context, CL_MEM_WRITE_ONLY, sizeof(cl_uint));
    cl::Kernel kernel(octreeProgram, "testLevelShift");
    cl_int3 ilo, ihi;
    ilo.x = ilox; ilo.y = iloy; ilo.z = iloz;
    ihi.x = ihix; ihi.y = ihiy; ihi.z = ihiz;
    kernel.setArg(0, out);
    kernel.setArg(1, ilo);
    kernel.setArg(2, ihi);
    queue.enqueueTask(kernel);
    queue.enqueueReadBuffer(out, CL_TRUE, 0, sizeof(cl_int), &ans);
    return ans;
}

float TestSplatTreeCL::callPointBoxDist2(float px, float py, float pz, float lx, float ly, float lz, float hx, float hy, float hz)
{
    cl_float ans;
    cl::Buffer out(context, CL_MEM_WRITE_ONLY, sizeof(cl_uint));
    cl::Kernel kernel(octreeProgram, "testPointBoxDist2");
    cl_float3 pos, lo, hi;
    pos.x = px; pos.y = py; pos.z = pz;
    lo.x = lx; lo.y = ly; lo.z = lz;
    hi.x = hx; hi.y = hy; hi.z = hz;
    kernel.setArg(0, out);
    kernel.setArg(1, pos);
    kernel.setArg(2, lo);
    kernel.setArg(3, hi);
    queue.enqueueTask(kernel);
    queue.enqueueReadBuffer(out, CL_TRUE, 0, sizeof(cl_float), &ans);
    return ans;
}

int TestSplatTreeCL::callMakeCode(cl_int x, cl_int y, cl_int z)
{
    cl_uint ans;
    cl::Buffer out(context, CL_MEM_WRITE_ONLY, sizeof(cl_uint));
    cl::Kernel kernel(octreeProgram, "testMakeCode");
    cl_int3 xyz;
    xyz.s0 = x; xyz.s1 = y; xyz.s2 = z;
    kernel.setArg(0, out);
    kernel.setArg(1, xyz);
    queue.enqueueTask(kernel);
    queue.enqueueReadBuffer(out, CL_TRUE, 0, sizeof(cl_uint), &ans);
    return ans;
}

float TestSplatTreeCL::callSolveQuadratic(float a, float b, float c)
{
    cl_float ans;
    cl::Buffer out(context, CL_MEM_WRITE_ONLY, sizeof(cl_float));
    cl::Kernel kernel(mlsProgram, "testSolveQuadratic");
    kernel.setArg(0, out);
    kernel.setArg(1, a);
    kernel.setArg(2, b);
    kernel.setArg(3, c);
    queue.enqueueTask(kernel);
    queue.enqueueReadBuffer(out, CL_TRUE, 0, sizeof(cl_float), &ans);
    return ans;
}

void TestSplatTreeCL::testLevelShift()
{
    CPPUNIT_ASSERT_EQUAL(0, callLevelShift(0, 0, 0,  0, 0, 0)); // single cell
    CPPUNIT_ASSERT_EQUAL(0, callLevelShift(1, 1, 1,  0, 0, 0)); // empty
    CPPUNIT_ASSERT_EQUAL(0, callLevelShift(0, 1, 2,  1, 2, 3)); // 2x2x2
    CPPUNIT_ASSERT_EQUAL(1, callLevelShift(0, 1, 2,  2, 2, 3)); // 3x2x2
    CPPUNIT_ASSERT_EQUAL(1, callLevelShift(0, 1, 2,  1, 3, 3)); // 2x3x2
    CPPUNIT_ASSERT_EQUAL(1, callLevelShift(0, 1, 2,  1, 2, 4)); // 2x2x3
    CPPUNIT_ASSERT_EQUAL(3, callLevelShift(31, 0, 0, 36, 0, 0)); // 011111 -> 100100
    CPPUNIT_ASSERT_EQUAL(3, callLevelShift(27, 0, 0, 32, 0, 0)); // 011011 -> 100000
    CPPUNIT_ASSERT_EQUAL(4, callLevelShift(48, 0, 0, 79, 0, 0)); // 0110000 -> 1001111
}

void TestSplatTreeCL::testPointBoxDist2()
{
    // Point inside the box
    CPPUNIT_ASSERT_DOUBLES_EQUAL(0.0,
        callPointBoxDist2(0.5f, 0.5f, 0.5f,  0.0f, 0.0f, 0.0f,  1.0f, 1.0f, 1.0f), 1e-4);
    // Above one face
    CPPUNIT_ASSERT_DOUBLES_EQUAL(4.0,
        callPointBoxDist2(0.25f, 0.5f, 3.0f,  -1.5f, 0.0f, 0.5f,  1.5f, 0.75f, 1.0f), 1e-4);
    // Nearest point is a corner
    CPPUNIT_ASSERT_DOUBLES_EQUAL(14.0,
        callPointBoxDist2(9.0f, 11.f, -10.f,  -1.0f, 0.0f, -7.0f,  8.0f, 9.0f, 8.0f), 1e-4);
}

void TestSplatTreeCL::testMakeCode()
{
    CPPUNIT_ASSERT_EQUAL(0, callMakeCode(0, 0, 0));
    CPPUNIT_ASSERT_EQUAL(7, callMakeCode(1, 1, 1));
    CPPUNIT_ASSERT_EQUAL(174, callMakeCode(2, 5, 3));
    CPPUNIT_ASSERT_EQUAL(511, callMakeCode(7, 7, 7));
}

void TestSplatTreeCL::testSolveQuadratic()
{
    float n = std::numeric_limits<float>::quiet_NaN();
    float eps = std::numeric_limits<float>::epsilon() * 4;

    // Cases with no roots
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(1, -2, 2), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(-1, 2, -2), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(1e20, -2e10, 1.0001), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(1, 0, 1), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(-1, 0, -1), eps);
    // Constant functions (no roots or infinitely many roots)
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(0, 0, 0), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(0, 0, 4), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(0, 0, -3), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(0, 0, -1e20), eps);
    ASSERT_DOUBLES_EQUAL(n, callSolveQuadratic(0, 0, 1e20), eps);
    // Linear functions
    ASSERT_DOUBLES_EQUAL(-1.5, callSolveQuadratic(0, 2, 3), eps);
    ASSERT_DOUBLES_EQUAL(2.5, callSolveQuadratic(0, -2, 5), eps);
    ASSERT_DOUBLES_EQUAL(0.0, callSolveQuadratic(0, 5, 0), eps);
    ASSERT_DOUBLES_EQUAL(0.0, callSolveQuadratic(0, 1e20, 0), eps);
    ASSERT_DOUBLES_EQUAL(0.0, callSolveQuadratic(0, 1e-20, 0), eps);
    ASSERT_DOUBLES_EQUAL(1e-20, callSolveQuadratic(0, 1e10, 1e-10), eps);
    ASSERT_DOUBLES_EQUAL(-1e20, callSolveQuadratic(0, 1e-10, 1e10), eps);
    // Repeated roots
    ASSERT_DOUBLES_EQUAL(1.0, callSolveQuadratic(1, -2, 1), eps);
    ASSERT_DOUBLES_EQUAL(1.0, callSolveQuadratic(10, -20, 10), eps);
    ASSERT_DOUBLES_EQUAL(1e4, callSolveQuadratic(1, -2e4, 1e8), eps);
    ASSERT_DOUBLES_EQUAL(0.0, callSolveQuadratic(1, 0, 0), eps);
    ASSERT_DOUBLES_EQUAL(0.0, callSolveQuadratic(1e30, 0, 0), eps);
    ASSERT_DOUBLES_EQUAL(0.0, callSolveQuadratic(1e-20, 0, 0), eps);
    // Regular two-root solutions
    ASSERT_DOUBLES_EQUAL(3.0, callSolveQuadratic(1, -5, 6), eps);
    ASSERT_DOUBLES_EQUAL(2.0, callSolveQuadratic(-2, 10, -12), eps);
    ASSERT_DOUBLES_EQUAL(2.0, callSolveQuadratic(1, 1, -6), eps);
    ASSERT_DOUBLES_EQUAL(-3.0, callSolveQuadratic(-0.1f, -0.1f, 0.6f), eps);
    ASSERT_DOUBLES_EQUAL(3.0, callSolveQuadratic(1e-12, -5e-12, 6e-12), eps);
    ASSERT_DOUBLES_EQUAL(-2e-12, callSolveQuadratic(1, 5e-12, 6e-24), eps);
    // Corner cases for stability
    ASSERT_DOUBLES_EQUAL(1.0, callSolveQuadratic(1, -1 - 1e-6, 1e-6), eps);
    ASSERT_DOUBLES_EQUAL(1e6, callSolveQuadratic(1, -1 - 1e6, 1e6), eps);
    ASSERT_DOUBLES_EQUAL(1e20, callSolveQuadratic(1e-20, -2, 1e20), eps);
    ASSERT_DOUBLES_EQUAL(-1e-6, callSolveQuadratic(1e-6, 1, 1e-6), eps);
}

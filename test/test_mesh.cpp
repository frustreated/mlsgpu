/**
 * @file
 *
 * Test code for @ref mesh.cpp.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>
#include <cstring>
#include <tr1/cstdint>
#include <map>
#include <algorithm>
#include <cstddef>
#include <boost/array.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/foreach.hpp>
#include <boost/array.hpp>
#include <CL/cl.hpp>
#include "testmain.h"
#include "../src/fast_ply.h"
#include "../src/mesh.h"
#include "../src/errors.h"
#include "test_clh.h"

using namespace std;

/**
 * An implementation of the @ref FastPly::WriterBase
 * interface that does not actually write to file, but merely saves
 * a copy of the data in memory. It is aimed specifically at testing.
 */
class MemoryWriter : public FastPly::WriterBase
{
public:
    /// Constructor
    MemoryWriter();

    virtual void open(const std::string &filename);
    virtual std::pair<char *, size_type> open();
    virtual void close();
    virtual void writeVertices(size_type first, size_type count, const float *data);
    virtual void writeTriangles(size_type first, size_type count, const std::tr1::uint32_t *data);
    virtual bool supportsOutOfOrder() const;

    const vector<boost::array<float, 3> > &getVertices() const { return vertices; }
    const vector<boost::array<std::tr1::uint32_t, 3> > &getTriangles() const { return triangles; }

private:
    vector<boost::array<float, 3> > vertices;
    vector<boost::array<std::tr1::uint32_t, 3> > triangles;
};

MemoryWriter::MemoryWriter()
{
}

void MemoryWriter::open(const std::string &filename)
{
    MLSGPU_ASSERT(!isOpen(), std::runtime_error);

    // Ignore the filename
    (void) filename;

    // NaN is tempting, but violates the Strict Weak Ordering requirements
    const boost::array<float, 3> badVertex = {{ -1000.0f, -1000.0f, -1000.0f }};
    const boost::array<std::tr1::uint32_t, 3> badTriangle = {{ UINT32_MAX, UINT32_MAX, UINT32_MAX }};
    vertices.resize(getNumVertices(), badVertex);
    triangles.resize(getNumTriangles(), badTriangle);
    setOpen(true);
}

std::pair<char *, MemoryWriter::size_type> MemoryWriter::open()
{
    MLSGPU_ASSERT(!isOpen(), std::runtime_error);

    vertices.resize(getNumVertices());
    triangles.resize(getNumTriangles());
    setOpen(true);

    return make_pair((char *) NULL, size_type(0));
}

void MemoryWriter::close()
{
    setOpen(false);
}

void MemoryWriter::writeVertices(size_type first, size_type count, const float *data)
{
    MLSGPU_ASSERT(isOpen(), std::runtime_error);
    MLSGPU_ASSERT(first + count <= getNumVertices() && first <= std::numeric_limits<size_type>::max() - count, std::out_of_range);

    std::memcpy(&vertices[first][0], data, count * 3 * sizeof(float));
}

void MemoryWriter::writeTriangles(size_type first, size_type count, const std::tr1::uint32_t *data)
{
    MLSGPU_ASSERT(isOpen(), std::runtime_error);
    MLSGPU_ASSERT(first + count <= getNumTriangles() && first <= std::numeric_limits<size_type>::max() - count, std::out_of_range);

    std::memcpy(&triangles[first][0], data, count * 3 * sizeof(std::tr1::uint32_t));
}

bool MemoryWriter::supportsOutOfOrder() const
{
    return true;
}


/**
 * Tests that are shared across all the @ref MeshBase subclasses, including those
 * that don't do welding.
 */
class TestMeshBase : public CLH::Test::TestFixture
{
    CPPUNIT_TEST_SUITE(TestMeshBase);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testNoInternal);
    CPPUNIT_TEST(testNoExternal);
    CPPUNIT_TEST(testEmpty);
    CPPUNIT_TEST_SUITE_END_ABSTRACT();
private:
    /**
     * Returns a rotation of the triangle to a canonical form.
     */
    boost::array<std::tr1::uint32_t, 3> canonicalTriangle(
        std::tr1::uint32_t idx0,
        std::tr1::uint32_t idx1,
        std::tr1::uint32_t idx2) const;

protected:
    virtual MeshBase *meshFactory(FastPly::WriterBase &writer) = 0;

    /**
     * Call the output functor with the data provided. This is a convenience
     * function which takes care of loading the data into OpenCL buffers.
     */
    void add(
        const Marching::OutputFunctor &functor,
        size_t numInternalVertices,
        size_t numExternalVertices,
        size_t numIndices,
        const boost::array<cl_float, 3> *internalVertices,
        const boost::array<cl_float, 3> *externalVertices,
        const cl_ulong *externalKeys,
        const cl_uint *indices);

    /**
     * Assert that the mesh produced is isomorphic to the data provided.
     * Is it permitted for the vertices and triangles to have been permuted and
     * for the order of indices in a triangle to have been rotated (but not
     * reflected).
     *
     * @pre The vertices are all unique.
     */
    void checkIsomorphic(
        size_t numVertices, size_t numIndices,
        const boost::array<cl_float, 3> *expectedVertices,
        const cl_uint *expectedIndices,
        const MemoryWriter &actual) const;


    /**
     * @name
     * @{
     * Test data.
     */
    static const boost::array<cl_float, 3> internalVertices0[];
    static const cl_uint indices0[];

    static const boost::array<cl_float, 3> externalVertices1[];
    static const cl_ulong externalKeys1[];
    static const cl_uint indices1[];

    static const boost::array<cl_float, 3> internalVertices2[];
    static const boost::array<cl_float, 3> externalVertices2[];
    static const cl_ulong externalKeys2[];
    static const cl_uint indices2[];

public:
    void testSimple();          ///< Normal uses cases
    void testNoInternal();      ///< An entire mesh with no internal vertices
    void testNoExternal();      ///< An entire mesh with no external vertices
    void testEmpty();           ///< Empty mesh
};

const boost::array<cl_float, 3> TestMeshBase::internalVertices0[] =
{
    {{ 0.0f, 0.0f, 1.0f }},
    {{ 0.0f, 0.0f, 2.0f }},
    {{ 0.0f, 0.0f, 3.0f }},
    {{ 0.0f, 0.0f, 4.0f }},
    {{ 0.0f, 0.0f, 5.0f }}
};
const cl_uint TestMeshBase::indices0[] =
{
    0, 1, 3,
    1, 2, 3,
    3, 4, 0
};

const boost::array<cl_float, 3> TestMeshBase::externalVertices1[] =
{
    {{ 1.0f, 0.0f, 1.0f }},
    {{ 1.0f, 0.0f, 2.0f }},
    {{ 1.0f, 0.0f, 3.0f }},
    {{ 1.0f, 0.0f, 4.0f }}
};
const cl_ulong TestMeshBase::externalKeys1[] =
{
    UINT64_C(0),
    UINT64_C(0x8000000000000000),
    UINT64_C(1),
    UINT64_C(0x8000000000000001)
};
const cl_uint TestMeshBase::indices1[] =
{
    0, 1, 3,
    1, 2, 3,
    2, 0, 3
};

const boost::array<cl_float, 3> TestMeshBase::internalVertices2[] =
{
    {{ 0.0f, 1.0f, 0.0f }},
    {{ 0.0f, 2.0f, 0.0f }},
    {{ 0.0f, 3.0f, 0.0f }}
};
const boost::array<cl_float, 3> TestMeshBase::externalVertices2[] =
{
    {{ 2.0f, 0.0f, 1.0f }},
    {{ 2.0f, 0.0f, 2.0f }}
};
const cl_ulong TestMeshBase::externalKeys2[] =
{
    UINT64_C(0x1234567812345678),
    UINT64_C(0x12345678)
};
const cl_uint TestMeshBase::indices2[] =
{
    0, 1, 3,
    1, 4, 3,
    2, 3, 4,
    0, 2, 4,
    0, 3, 2
};


boost::array<std::tr1::uint32_t, 3> TestMeshBase::canonicalTriangle(
    std::tr1::uint32_t idx0,
    std::tr1::uint32_t idx1,
    std::tr1::uint32_t idx2) const
{
    boost::array<std::tr1::uint32_t, 3> rot[3] =
    {
        {{ idx0, idx1, idx2 }},
        {{ idx1, idx2, idx0 }},
        {{ idx2, idx0, idx1 }}
    };
    return *min_element(rot, rot + 3);
}

void TestMeshBase::add(
    const Marching::OutputFunctor &functor,
    size_t numInternalVertices,
    size_t numExternalVertices,
    size_t numIndices,
    const boost::array<cl_float, 3> *internalVertices,
    const boost::array<cl_float, 3> *externalVertices,
    const cl_ulong *externalKeys,
    const cl_uint *indices)
{
    size_t numVertices = numInternalVertices + numExternalVertices;
    assert(numVertices > 0 && numIndices > 0);

    cl::Buffer dVertices(context, CL_MEM_READ_WRITE, numVertices * 3 * sizeof(cl_float));
    cl::Buffer dVertexKeys(context, CL_MEM_READ_WRITE, numVertices * sizeof(cl_ulong));
    cl::Buffer dIndices(context, CL_MEM_READ_WRITE, numIndices * sizeof(cl_uint));
    if (numInternalVertices > 0)
    {
        queue.enqueueWriteBuffer(dVertices, CL_FALSE,
                                 0,
                                 numInternalVertices * 3 * sizeof(cl_float),
                                 internalVertices);
    }
    if (numExternalVertices > 0)
    {
        queue.enqueueWriteBuffer(dVertices, CL_FALSE,
                                 numInternalVertices * 3 * sizeof(cl_float),
                                 numExternalVertices * 3 * sizeof(cl_float),
                                 externalVertices);
        queue.enqueueWriteBuffer(dVertexKeys, CL_FALSE,
                                 numInternalVertices * sizeof(cl_ulong),
                                 numExternalVertices * sizeof(cl_ulong),
                                 externalKeys);
    }
    queue.enqueueWriteBuffer(dIndices, CL_FALSE,
                             0,
                             numIndices * sizeof(cl_uint),
                             indices);
    queue.finish();

    functor(queue, dVertices, dVertexKeys, dIndices,
            numVertices, numInternalVertices, numIndices, NULL);
}

void TestMeshBase::checkIsomorphic(
    size_t numVertices, size_t numIndices,
    const boost::array<cl_float, 3> *expectedVertices,
    const cl_uint *expectedIndices,
    const MemoryWriter &actual) const
{
    const vector<boost::array<float, 3> > &actualVertices = actual.getVertices();
    const vector<boost::array<std::tr1::uint32_t, 3> > &actualTriangles = actual.getTriangles();
    CPPUNIT_ASSERT_EQUAL(numVertices, actualVertices.size());
    CPPUNIT_ASSERT_EQUAL(numIndices, 3 * actualTriangles.size());

    // Maps vertex data to its position in the expectedVertices list
    map<boost::array<float, 3>, size_t> vertexMap;
    // Maps triangle in canonical form to number of occurrences in expectedTriangles list
    map<boost::array<std::tr1::uint32_t, 3>, size_t> triangleMap;
    for (size_t i = 0; i < numVertices; i++)
    {
        boost::array<float, 3> v = expectedVertices[i];
        bool added = vertexMap.insert(make_pair(v, i)).second;
        CPPUNIT_ASSERT_MESSAGE("Vertices must be unique", added);
    }

    for (size_t i = 0; i < numIndices; i += 3)
    {
        const boost::array<std::tr1::uint32_t, 3> canon
            = canonicalTriangle(expectedIndices[i],
                                expectedIndices[i + 1],
                                expectedIndices[i + 2]);
        ++triangleMap[canon];
    }

    // Check that each vertex has a match. It is not necessary to check for
    // duplicate vertices because we've already checked for equal counts.
    for (size_t i = 0; i < numVertices; i++)
    {
        CPPUNIT_ASSERT(vertexMap.count(actualVertices[i]));
    }

    // Match up the actual triangles against the expected ones
    for (size_t i = 0; i < actualTriangles.size(); i++)
    {
        boost::array<std::tr1::uint32_t, 3> triangle = actualTriangles[i];
        for (int j = 0; j < 3; j++)
        {
            CPPUNIT_ASSERT(triangle[j] < numVertices);
            triangle[j] = vertexMap[actualVertices[triangle[j]]];
        }
        triangle = canonicalTriangle(triangle[0], triangle[1], triangle[2]);
        --triangleMap[triangle];
    }

    pair<boost::array<std::tr1::uint32_t, 3>, size_t> i;
    BOOST_FOREACH(i, triangleMap)
    {
        CPPUNIT_ASSERT_MESSAGE("Triangle mismatch", i.second == 0);
    }
}

void TestMeshBase::testSimple()
{
    const boost::array<cl_float, 3> expectedVertices[] =
    {
        {{ 0.0f, 0.0f, 1.0f }},
        {{ 0.0f, 0.0f, 2.0f }},
        {{ 0.0f, 0.0f, 3.0f }},
        {{ 0.0f, 0.0f, 4.0f }},
        {{ 0.0f, 0.0f, 5.0f }},
        {{ 1.0f, 0.0f, 1.0f }},
        {{ 1.0f, 0.0f, 2.0f }},
        {{ 1.0f, 0.0f, 3.0f }},
        {{ 1.0f, 0.0f, 4.0f }},
        {{ 0.0f, 1.0f, 0.0f }},
        {{ 0.0f, 2.0f, 0.0f }},
        {{ 0.0f, 3.0f, 0.0f }},
        {{ 2.0f, 0.0f, 1.0f }},
        {{ 2.0f, 0.0f, 2.0f }}
    };
    const cl_uint expectedIndices[] =
    {
        0, 1, 3,
        1, 2, 3,
        3, 4, 0,
        5, 6, 8,
        6, 7, 8,
        7, 5, 8,
        9, 10, 12,
        10, 13, 12,
        11, 12, 13,
        9, 11, 13,
        9, 12, 11
    };

    MemoryWriter writer;
    boost::scoped_ptr<MeshBase> mesh(meshFactory(writer));
    unsigned int passes = mesh->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        Marching::OutputFunctor functor = mesh->outputFunctor(i);
        /* Reverse the order on each pass, to ensure that the mesh
         * classes are robust to non-deterministic reordering.
         */
        if (i % 2 == 0)
        {
            add(functor,
                boost::size(internalVertices0), 0, boost::size(indices0),
                internalVertices0, NULL, NULL, indices0);
            add(functor,
                0, boost::size(externalVertices1), boost::size(indices1),
                NULL, externalVertices1, externalKeys1, indices1);
            add(functor,
                boost::size(internalVertices2),
                boost::size(externalVertices2),
                boost::size(indices2),
                internalVertices2, externalVertices2, externalKeys2, indices2);
        }
        else
        {
            add(functor,
                boost::size(internalVertices2),
                boost::size(externalVertices2),
                boost::size(indices2),
                internalVertices2, externalVertices2, externalKeys2, indices2);
            add(functor,
                0, boost::size(externalVertices1), boost::size(indices1),
                NULL, externalVertices1, externalKeys1, indices1);
            add(functor,
                boost::size(internalVertices0), 0, boost::size(indices0),
                internalVertices0, NULL, NULL, indices0);
        }
    }
    mesh->finalize();
    mesh->write(writer, "");

    // Check that boost::size really works on these arrays
    CPPUNIT_ASSERT_EQUAL(5, int(boost::size(internalVertices0)));

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer);
}

void TestMeshBase::testNoInternal()
{
    // Shadows the class version, which is for internal+external.
    const cl_uint indices2[] =
    {
        0, 1, 1,
        0, 0, 1
    };

    const boost::array<float, 3> expectedVertices[] =
    {
        {{ 1.0f, 0.0f, 1.0f }},
        {{ 1.0f, 0.0f, 2.0f }},
        {{ 1.0f, 0.0f, 3.0f }},
        {{ 1.0f, 0.0f, 4.0f }},
        {{ 2.0f, 0.0f, 1.0f }},
        {{ 2.0f, 0.0f, 2.0f }}
    };

    const cl_uint expectedIndices[] =
    {
        0, 1, 3,
        1, 2, 3,
        2, 0, 3,
        4, 5, 5,
        4, 4, 5
    };

    MemoryWriter writer;
    boost::scoped_ptr<MeshBase> mesh(meshFactory(writer));
    unsigned int passes = mesh->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        Marching::OutputFunctor functor = mesh->outputFunctor(i);
        add(functor,
            0, boost::size(externalVertices1), boost::size(indices1),
            NULL, externalVertices1, externalKeys1, indices1);
        add(functor,
            0,
            boost::size(externalVertices2),
            boost::size(indices2),
            NULL, externalVertices2, externalKeys2, indices2);
    }
    mesh->finalize();
    mesh->write(writer, "");

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer);
}

void TestMeshBase::testNoExternal()
{
    // Shadows the class version, which is for internal+external.
    const cl_uint indices2[] =
    {
        0, 1, 2,
        2, 1, 0
    };

    const boost::array<float, 3> expectedVertices[] =
    {
        {{ 0.0f, 0.0f, 1.0f }},
        {{ 0.0f, 0.0f, 2.0f }},
        {{ 0.0f, 0.0f, 3.0f }},
        {{ 0.0f, 0.0f, 4.0f }},
        {{ 0.0f, 0.0f, 5.0f }},
        {{ 0.0f, 1.0f, 0.0f }},
        {{ 0.0f, 2.0f, 0.0f }},
        {{ 0.0f, 3.0f, 0.0f }}
    };

    const cl_uint expectedIndices[] =
    {
        0, 1, 3,
        1, 2, 3,
        3, 4, 0,
        5, 6, 7,
        7, 6, 5
    };

    MemoryWriter writer;
    boost::scoped_ptr<MeshBase> mesh(meshFactory(writer));
    unsigned int passes = mesh->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        Marching::OutputFunctor functor = mesh->outputFunctor(i);
        add(functor,
            boost::size(internalVertices0), 0, boost::size(indices0),
            internalVertices0, NULL, NULL, indices0);
        add(functor,
            boost::size(internalVertices2),
            0, 
            boost::size(indices2),
            internalVertices2, NULL, NULL, indices2);
    }
    mesh->finalize();
    mesh->write(writer, "");

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer);
}

void TestMeshBase::testEmpty()
{
    MemoryWriter writer;
    boost::scoped_ptr<MeshBase> mesh(meshFactory(writer));
    unsigned int passes = mesh->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        Marching::OutputFunctor functor = mesh->outputFunctor(i);
    }
    mesh->finalize();
    mesh->write(writer, "");

    CPPUNIT_ASSERT(writer.getVertices().empty());
    CPPUNIT_ASSERT(writer.getTriangles().empty());
}

class TestSimpleMesh : public TestMeshBase
{
    CPPUNIT_TEST_SUB_SUITE(TestSimpleMesh, TestMeshBase);
    CPPUNIT_TEST_SUITE_END();
protected:
    virtual MeshBase *meshFactory(FastPly::WriterBase &writer);
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestSimpleMesh, TestSet::perBuild());

MeshBase *TestSimpleMesh::meshFactory(FastPly::WriterBase &)
{
    return new SimpleMesh();
}

class TestWeldMesh : public TestMeshBase
{
    CPPUNIT_TEST_SUB_SUITE(TestWeldMesh, TestMeshBase);
    CPPUNIT_TEST(testWeld);
    CPPUNIT_TEST_SUITE_END();
protected:
    virtual MeshBase *meshFactory(FastPly::WriterBase &writer);

    static const boost::array<cl_float, 3> internalVertices3[];
    static const boost::array<cl_float, 3> externalVertices3[];
    static const cl_ulong externalKeys3[];
    static const cl_uint indices3[];
public:
    void testWeld();     ///< Tests vertex welding
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestWeldMesh, TestSet::perBuild());

const boost::array<cl_float, 3> TestWeldMesh::internalVertices3[] =
{
    {{ 3.0f, 3.0f, 3.0f }}
};

const boost::array<cl_float, 3> TestWeldMesh::externalVertices3[] =
{
    {{ 4.0f, 5.0f, 6.0f }},
    {{ 1.0f, 0.0f, 2.0f }},
    {{ 1.0f, 0.0f, 3.0f }},
    {{ 2.0f, 0.0f, 2.0f }}
};

const cl_ulong TestWeldMesh::externalKeys3[] =
{
    100,
    UINT64_C(0x8000000000000000),   // shared with externalKeys1
    UINT64_C(1),                    // shared with externalKeys1
    UINT64_C(0x12345678)            // shared with externalKeys2
};

const cl_uint TestWeldMesh::indices3[] =
{
    0, 2, 1,
    1, 2, 4,
    4, 2, 3
};

void TestWeldMesh::testWeld()
{
    const boost::array<cl_float, 3> expectedVertices[] =
    {
        {{ 0.0f, 0.0f, 1.0f }},
        {{ 0.0f, 0.0f, 2.0f }},
        {{ 0.0f, 0.0f, 3.0f }},
        {{ 0.0f, 0.0f, 4.0f }},
        {{ 0.0f, 0.0f, 5.0f }},
        {{ 1.0f, 0.0f, 1.0f }},
        {{ 1.0f, 0.0f, 2.0f }},
        {{ 1.0f, 0.0f, 3.0f }},
        {{ 1.0f, 0.0f, 4.0f }},
        {{ 0.0f, 1.0f, 0.0f }},
        {{ 0.0f, 2.0f, 0.0f }},
        {{ 0.0f, 3.0f, 0.0f }},
        {{ 2.0f, 0.0f, 1.0f }},
        {{ 2.0f, 0.0f, 2.0f }},
        {{ 3.0f, 3.0f, 3.0f }},
        {{ 4.0f, 5.0f, 6.0f }}
    };
    const cl_uint expectedIndices[] =
    {
        0, 1, 3,
        1, 2, 3,
        3, 4, 0,
        5, 6, 8,
        6, 7, 8,
        7, 5, 8,
        9, 10, 12,
        10, 13, 12,
        11, 12, 13,
        9, 11, 13,
        9, 12, 11,
        14, 6, 15,
        15, 6, 13,
        13, 6, 7
    };

    MemoryWriter writer;
    boost::scoped_ptr<MeshBase> mesh(meshFactory(writer));
    unsigned int passes = mesh->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        Marching::OutputFunctor functor = mesh->outputFunctor(i);
        add(functor,
            boost::size(internalVertices0), 0, boost::size(indices0),
            internalVertices0, NULL, NULL, indices0);
        add(functor,
            0, boost::size(externalVertices1), boost::size(indices1),
            NULL, externalVertices1, externalKeys1, indices1);
        add(functor,
            boost::size(internalVertices2),
            boost::size(externalVertices2),
            boost::size(indices2),
            internalVertices2, externalVertices2, externalKeys2, indices2);
        add(functor,
            boost::size(internalVertices3),
            boost::size(externalVertices3),
            boost::size(indices3),
            internalVertices3, externalVertices3, externalKeys3, indices3);
    }
    mesh->finalize();
    mesh->write(writer, "");

    // Check that boost::size really works on these arrays
    CPPUNIT_ASSERT_EQUAL(9, int(boost::size(indices3)));

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer);
}

MeshBase *TestWeldMesh::meshFactory(FastPly::WriterBase &)
{
    return new WeldMesh();
}

class TestBigMesh : public TestWeldMesh
{
    CPPUNIT_TEST_SUB_SUITE(TestBigMesh, TestWeldMesh);
    CPPUNIT_TEST_SUITE_END();
protected:
    virtual MeshBase *meshFactory(FastPly::WriterBase &writer);
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestBigMesh, TestSet::perBuild());

MeshBase *TestBigMesh::meshFactory(FastPly::WriterBase &writer)
{
    return new BigMesh(writer, "");
}

class TestStxxlMesh : public TestWeldMesh
{
    CPPUNIT_TEST_SUB_SUITE(TestStxxlMesh, TestWeldMesh);
    CPPUNIT_TEST_SUITE_END();
protected:
    virtual MeshBase *meshFactory(FastPly::WriterBase &writer);
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestStxxlMesh, TestSet::perBuild());

MeshBase *TestStxxlMesh::meshFactory(FastPly::WriterBase &)
{
    return new StxxlMesh();
}

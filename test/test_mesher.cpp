/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Test code for @ref mesher.cpp.
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
#include "../src/tr1_cstdint.h"
#include <boost/tr1/random.hpp>
#include "../src/tr1_unordered_map.h"
#include "../src/tr1_unordered_set.h"
#include <map>
#include <algorithm>
#include <iterator>
#include <cstddef>
#include <boost/array.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/scoped_array.hpp>
#include <boost/smart_ptr/shared_array.hpp>
#include <boost/foreach.hpp>
#include <boost/array.hpp>
#include <boost/filesystem.hpp>
#include <CL/cl.hpp>
#include "testutil.h"
#include "../src/fast_ply.h"
#include "../src/mesher.h"
#include "test_clh.h"
#include "memory_reader.h"
#include "memory_writer.h"

using namespace std;

/// Unit test for @ref TrivialNamer
class TestTrivialNamer : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestTrivialNamer);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST_SUITE_END();
public:
    void testSimple();
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestTrivialNamer, TestSet::perBuild());

void TestTrivialNamer::testSimple()
{
    ChunkId chunkId;
    chunkId.gen = 123;
    chunkId.coords[0] = 1;
    chunkId.coords[1] = 2;
    chunkId.coords[2] = 3;
    TrivialNamer namer("foo.ply");
    CPPUNIT_ASSERT_EQUAL(string("foo.ply"), namer(chunkId));
}

class TestChunkNamer : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestChunkNamer);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testBig);
    CPPUNIT_TEST_SUITE_END();
public:
    void testSimple();   ///< Test normal usage
    void testBig();      ///< Test with values that overflow field width
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestChunkNamer, TestSet::perBuild());

void TestChunkNamer::testSimple()
{
    ChunkId chunkId;
    chunkId.gen = 123;
    chunkId.coords[0] = 0;
    chunkId.coords[1] = 5;
    chunkId.coords[2] = 3000;
    ChunkNamer namer("foo");
    CPPUNIT_ASSERT_EQUAL(string("foo_0000_0005_3000.ply"), namer(chunkId));
}

void TestChunkNamer::testBig()
{
    ChunkId chunkId;
    chunkId.gen = 123;
    chunkId.coords[0] = 100;
    chunkId.coords[1] = 123456;
    chunkId.coords[2] = 2345678;
    ChunkNamer namer("foo");
    CPPUNIT_ASSERT_EQUAL(string("foo_0100_123456_2345678.ply"), namer(chunkId));
}

/**
 * Tests that are shared across all the @ref MesherBase subclasses.
 */
class TestMesherBase : public CLH::Test::TestFixture
{
    CPPUNIT_TEST_SUITE(TestMesherBase);
    CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST(testNoInternal);
    CPPUNIT_TEST(testNoExternal);
    CPPUNIT_TEST(testEmpty);
    CPPUNIT_TEST(testWeld);
    CPPUNIT_TEST(testPrune);
    CPPUNIT_TEST(testChunk);
    // CPPUNIT_TEST(testRandom); // Moved to TestMesherBaseSlow
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
    virtual MesherBase *mesherFactory(FastPly::Writer &writer, const MesherBase::Namer &namer) = 0;

    MesherBase *mesherFactory(FastPly::Writer &writer)
    {
        return mesherFactory(writer, TrivialNamer(""));
    }

    /**
     * Call the output functor with the data provided. This is a convenience
     * function which takes care of loading the data into OpenCL buffers.
     */
    void add(
        const ChunkId &chunkId,
        const MesherBase::InputFunctor &functor,
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
        const std::string &actualRaw) const;

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

    static const boost::array<cl_float, 3> internalVertices3[];
    static const boost::array<cl_float, 3> externalVertices3[];
    static const cl_ulong externalKeys3[];
    static const cl_uint indices3[];

    struct Component
    {
        int width;
        int height;
        std::vector<cl_ulong> vertices;  // actually keys
        std::vector<boost::array<cl_ulong, 3> > triangles;
        // Number of blocks that contain the vertex (to determine whether it
        // is external).
        std::vector<int> vertexOwners;
    };

    struct Block
    {
        std::tr1::unordered_set<cl_ulong> vertices;
        std::vector<boost::array<cl_ulong, 3> > triangles;
        boost::shared_array<char> buffer; // storage for work
        MesherWork work;
    };

    struct Chunk
    {
        ChunkId id;
        std::vector<Block> blocks;
        std::vector<boost::array<float, 3> > expectedVertices;
        std::vector<boost::array<cl_uint, 3> > expectedTriangles;
        std::tr1::unordered_map<cl_ulong, std::size_t> indices;
    };

    struct Vertex
    {
        boost::array<float, 3> coords;
        unsigned int owners;
    };

public:
    void testSimple();          ///< Normal uses cases
    void testNoInternal();      ///< An entire mesh with no internal vertices
    void testNoExternal();      ///< An entire mesh with no external vertices
    void testEmpty();           ///< Empty mesh
    void testWeld();            ///< Tests vertex welding
    void testPrune();           ///< Tests component pruning
    void testChunk();           ///< Test chunking into multiple files
    void testRandom();          ///< Test with pseudo-random data
};

const boost::array<cl_float, 3> TestMesherBase::internalVertices0[] =
{
    {{ 0.0f, 0.0f, 1.0f }},
    {{ 0.0f, 0.0f, 2.0f }},
    {{ 0.0f, 0.0f, 3.0f }},
    {{ 0.0f, 0.0f, 4.0f }},
    {{ 0.0f, 0.0f, 5.0f }}
};
const cl_uint TestMesherBase::indices0[] =
{
    0, 1, 3,
    1, 2, 3,
    3, 4, 0
};

const boost::array<cl_float, 3> TestMesherBase::externalVertices1[] =
{
    {{ 1.0f, 0.0f, 1.0f }},
    {{ 1.0f, 0.0f, 2.0f }},
    {{ 1.0f, 0.0f, 3.0f }},
    {{ 1.0f, 0.0f, 4.0f }}
};
const cl_ulong TestMesherBase::externalKeys1[] =
{
    UINT64_C(0),
    UINT64_C(0x8000000000000000),
    UINT64_C(1),
    UINT64_C(0x8000000000000001)
};
const cl_uint TestMesherBase::indices1[] =
{
    0, 1, 3,
    1, 2, 3,
    2, 0, 3
};

const boost::array<cl_float, 3> TestMesherBase::internalVertices2[] =
{
    {{ 0.0f, 1.0f, 0.0f }},
    {{ 0.0f, 2.0f, 0.0f }},
    {{ 0.0f, 3.0f, 0.0f }}
};
const boost::array<cl_float, 3> TestMesherBase::externalVertices2[] =
{
    {{ 2.0f, 0.0f, 1.0f }},
    {{ 2.0f, 0.0f, 2.0f }}
};
const cl_ulong TestMesherBase::externalKeys2[] =
{
    UINT64_C(0x1234567812345678),
    UINT64_C(0x12345678)
};
const cl_uint TestMesherBase::indices2[] =
{
    0, 1, 3,
    1, 4, 3,
    2, 3, 4,
    0, 2, 4,
    0, 3, 2
};

const boost::array<cl_float, 3> TestMesherBase::internalVertices3[] =
{
    {{ 3.0f, 3.0f, 3.0f }}
};

const boost::array<cl_float, 3> TestMesherBase::externalVertices3[] =
{
    {{ 4.0f, 5.0f, 6.0f }},
    {{ 1.0f, 0.0f, 2.0f }},
    {{ 1.0f, 0.0f, 3.0f }},
    {{ 2.0f, 0.0f, 2.0f }}
};

const cl_ulong TestMesherBase::externalKeys3[] =
{
    100,
    UINT64_C(0x8000000000000000),   // shared with externalKeys1
    UINT64_C(1),                    // shared with externalKeys1
    UINT64_C(0x12345678)            // shared with externalKeys2
};

const cl_uint TestMesherBase::indices3[] =
{
    0, 2, 1,
    1, 2, 4,
    4, 2, 3
};


boost::array<std::tr1::uint32_t, 3> TestMesherBase::canonicalTriangle(
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

void TestMesherBase::add(
    const ChunkId &chunkId,
    const MesherBase::InputFunctor &functor,
    size_t numInternalVertices,
    size_t numExternalVertices,
    size_t numIndices,
    const boost::array<cl_float, 3> *internalVertices,
    const boost::array<cl_float, 3> *externalVertices,
    const cl_ulong *externalKeys,
    const cl_uint *indices)
{
    Timeplot::Worker tworker("test");

    const size_t numTriangles = numIndices / 3;
    const size_t numVertices = numInternalVertices + numExternalVertices;
    assert(numIndices % 3 == 0);

    MeshSizes sizes(numVertices, numTriangles, numInternalVertices);
    boost::scoped_array<char> buffer(new char[sizes.getHostBytes()]);
    MesherWork work;
    work.mesh = HostKeyMesh(buffer.get(), sizes);
    std::copy(internalVertices, internalVertices + numInternalVertices,
              work.mesh.vertices);
    std::copy(externalVertices, externalVertices + numExternalVertices,
              work.mesh.vertices + numInternalVertices);
    std::copy(externalKeys, externalKeys + numExternalVertices,
              work.mesh.vertexKeys);
    for (std::size_t i = 0; i < numTriangles; i++)
    {
        boost::array<cl_uint, 3> triangle;
        for (unsigned int j = 0; j < 3; j++)
            triangle[j] = indices[i * 3 + j];
        work.mesh.triangles[i] = triangle;
    }

    // Create already-signaled events
    CLH::enqueueMarkerWithWaitList(queue, NULL, &work.verticesEvent);
    CLH::enqueueMarkerWithWaitList(queue, NULL, &work.vertexKeysEvent);
    CLH::enqueueMarkerWithWaitList(queue, NULL, &work.trianglesEvent);
    work.hasEvents = true;
    queue.flush();

    work.chunkId = chunkId;
    functor(work, tworker);
}

void TestMesherBase::checkIsomorphic(
    size_t numVertices, size_t numIndices,
    const boost::array<cl_float, 3> *expectedVertices,
    const cl_uint *expectedIndices,
    const std::string &actualRaw) const
{
    vector<boost::array<float, 3> > actualVertices;
    vector<boost::array<std::tr1::uint32_t, 3> > actualTriangles;

    MemoryWriterPly::parse(actualRaw, actualVertices, actualTriangles);

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

void TestMesherBase::testSimple()
{
    Timeplot::Worker tworker("test");

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

    MemoryWriterPly writer;

    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer));
    unsigned int passes = mesher->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(i);
        /* Reverse the order on each pass, to ensure that the mesher
         * classes are robust to non-deterministic reordering.
         */
        if (i % 2 == 0)
        {
            add(ChunkId(), functor,
                boost::size(internalVertices0), 0, boost::size(indices0),
                internalVertices0, NULL, NULL, indices0);
            add(ChunkId(), functor,
                0, boost::size(externalVertices1), boost::size(indices1),
                NULL, externalVertices1, externalKeys1, indices1);
            add(ChunkId(), functor,
                boost::size(internalVertices2),
                boost::size(externalVertices2),
                boost::size(indices2),
                internalVertices2, externalVertices2, externalKeys2, indices2);
        }
        else
        {
            add(ChunkId(), functor,
                boost::size(internalVertices2),
                boost::size(externalVertices2),
                boost::size(indices2),
                internalVertices2, externalVertices2, externalKeys2, indices2);
            add(ChunkId(), functor,
                0, boost::size(externalVertices1), boost::size(indices1),
                NULL, externalVertices1, externalKeys1, indices1);
            add(ChunkId(), functor,
                boost::size(internalVertices0), 0, boost::size(indices0),
                internalVertices0, NULL, NULL, indices0);
        }
    }
    mesher->write(tworker);

    // Check that boost::size really works on these arrays
    MLSGPU_ASSERT_EQUAL(5, boost::size(internalVertices0));

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer.getOutput(""));
}

void TestMesherBase::testNoInternal()
{
    Timeplot::Worker tworker("test");

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

    MemoryWriterPly writer;

    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer));
    unsigned int passes = mesher->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(i);
        add(ChunkId(), functor,
            0, boost::size(externalVertices1), boost::size(indices1),
            NULL, externalVertices1, externalKeys1, indices1);
        add(ChunkId(), functor,
            0,
            boost::size(externalVertices2),
            boost::size(indices2),
            NULL, externalVertices2, externalKeys2, indices2);
    }
    mesher->write(tworker);

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer.getOutput(""));
}

void TestMesherBase::testNoExternal()
{
    Timeplot::Worker tworker("test");

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

    MemoryWriterPly writer;

    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer));
    unsigned int passes = mesher->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(i);
        add(ChunkId(), functor,
            boost::size(internalVertices0), 0, boost::size(indices0),
            internalVertices0, NULL, NULL, indices0);
        add(ChunkId(), functor,
            boost::size(internalVertices2),
            0,
            boost::size(indices2),
            internalVertices2, NULL, NULL, indices2);
    }
    mesher->write(tworker);

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer.getOutput(""));
}

void TestMesherBase::testEmpty()
{
    Timeplot::Worker tworker("test");

    MemoryWriterPly writer;

    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer));
    unsigned int passes = mesher->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(i);
        add(ChunkId(), functor, 0, 0, 0, NULL, NULL, NULL, NULL);
    }
    mesher->write(tworker);

    // Output should not be produced for empty chunks
    CPPUNIT_ASSERT_THROW(writer.getOutput(""), std::invalid_argument);
}

void TestMesherBase::testWeld()
{
    Timeplot::Worker tworker("test");

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

    MemoryWriterPly writer;

    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer));
    unsigned int passes = mesher->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(i);
        add(ChunkId(), functor,
            boost::size(internalVertices0), 0, boost::size(indices0),
            internalVertices0, NULL, NULL, indices0);
        add(ChunkId(), functor,
            0, boost::size(externalVertices1), boost::size(indices1),
            NULL, externalVertices1, externalKeys1, indices1);
        add(ChunkId(), functor,
            boost::size(internalVertices2),
            boost::size(externalVertices2),
            boost::size(indices2),
            internalVertices2, externalVertices2, externalKeys2, indices2);
        add(ChunkId(), functor,
            boost::size(internalVertices3),
            boost::size(externalVertices3),
            boost::size(indices3),
            internalVertices3, externalVertices3, externalKeys3, indices3);
    }
    mesher->write(tworker);

    // Check that boost::size really works on these arrays
    CPPUNIT_ASSERT_EQUAL(9, int(boost::size(indices3)));

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer.getOutput(""));
}

void TestMesherBase::testPrune()
{
    Timeplot::Worker tworker("test");

    /* There are several cases to test:
     * - A: Component entirely contained in one block, undersized: 5 vertices in block 0.
     * - B: Component entirely contained in one block, large enough: 6 vertices in block 1.
     * - C: Component split across blocks, whole component is undersized: 5 vertices split
     *   between blocks 2 and 3.
     * - D: Component split across blocks, some clumps of undersized but component
     *   is large enough: 6 vertices split between blocks 0-3.
     *
     * The component of each vertex is indicated in the Y coordinate (0 = A etc). The
     * X coordinate indexes within the component, and Z is zero. External keys follow
     * a similar scheme, with the component given by the upper nibble.
     */
    const boost::array<cl_float, 3> internalVertices0[] =
    {
        {{ 0.0f, 0.0f, 0.0f }}, // 0
        {{ 1.0f, 0.0f, 0.0f }}, // 1
        {{ 2.0f, 0.0f, 0.0f }}, // 2
        {{ 3.0f, 0.0f, 0.0f }}, // 3
        {{ 4.0f, 0.0f, 0.0f }}  // 4
    };
    const boost::array<cl_float, 3> externalVertices0[] =
    {
        {{ 0.0f, 3.0f, 0.0f }}, // 5
        {{ 1.0f, 3.0f, 0.0f }}, // 6
        {{ 2.0f, 3.0f, 0.0f }}  // 7
    };
    const cl_ulong externalKeys0[] =
    {
        0x30, 0x31, 0x32
    };
    const cl_uint indices0[] =
    {
        0, 4, 1,
        1, 4, 2,
        2, 4, 3,
        5, 7, 6
    };

    const boost::array<cl_float, 3> internalVertices1[] =
    {
        {{ 0.0f, 1.0f, 0.0f }}, // 0
        {{ 1.0f, 1.0f, 0.0f }}, // 1
        {{ 2.0f, 1.0f, 0.0f }}, // 2
        {{ 3.0f, 1.0f, 0.0f }}, // 3
        {{ 4.0f, 1.0f, 0.0f }}, // 4
        {{ 5.0f, 1.0f, 0.0f }}, // 5

        {{ 0.0f, 2.0f, 0.0f }}, // 6
        {{ 3.0f, 2.0f, 0.0f }}  // 7
    };
    const boost::array<cl_float, 3> externalVertices1[] =
    {
        {{ 2.0f, 2.0f, 0.0f }}, // 8
        {{ 4.0f, 2.0f, 0.0f }}, // 9
        {{ 0.0f, 3.0f, 0.0f }}, // 10
        {{ 2.0f, 3.0f, 0.0f }}, // 11
        {{ 4.0f, 3.0f, 0.0f }}  // 12
    };
    const cl_ulong externalKeys1[] =
    {
        0x22, 0x24, 0x30, 0x32, 0x34
    };
    const cl_uint indices1[] =
    {
        0, 5, 1,
        1, 5, 2,
        2, 5, 3,
        3, 5, 4,
        6, 7, 9,
        9, 7, 8,
        10, 12, 11
    };

    // No internal vertices in block 2
    const boost::array<cl_float, 3> externalVertices2[] =
    {
        {{ 1.0f, 3.0f, 0.0f }},
        {{ 2.0f, 3.0f, 0.0f }},
        {{ 3.0f, 3.0f, 0.0f }}
    };
    const cl_ulong externalKeys2[] =
    {
        0x31, 0x32, 0x33
    };
    const cl_uint indices2[] =
    {
        0, 1, 2
    };

    const boost::array<cl_float, 3> internalVertices3[] =
    {
        {{ 1.0f, 2.0f, 0.0f }}, // 0
        {{ 5.0f, 3.0f, 0.0f }}  // 1
    };
    const boost::array<cl_float, 3> externalVertices3[] =
    {
        {{ 2.0f, 2.0f, 0.0f }}, // 2
        {{ 3.0f, 3.0f, 0.0f }}, // 3
        {{ 4.0f, 2.0f, 0.0f }}, // 4
        {{ 4.0f, 3.0f, 0.0f }}, // 5
        {{ 2.0f, 3.0f, 0.0f }}  // 6
    };
    const cl_ulong externalKeys3[] =
    {
        0x22, 0x33, 0x24, 0x34, 0x32
    };
    const cl_uint indices3[] =
    {
        6, 5, 3,
        4, 2, 0,
        3, 5, 1
    };

    const boost::array<cl_float, 3> expectedVertices[] =
    {
        {{ 0.0f, 1.0f, 0.0f }}, // 0
        {{ 1.0f, 1.0f, 0.0f }}, // 1
        {{ 2.0f, 1.0f, 0.0f }}, // 2
        {{ 3.0f, 1.0f, 0.0f }}, // 3
        {{ 4.0f, 1.0f, 0.0f }}, // 4
        {{ 5.0f, 1.0f, 0.0f }}, // 5
        {{ 0.0f, 3.0f, 0.0f }}, // 6
        {{ 1.0f, 3.0f, 0.0f }}, // 7
        {{ 2.0f, 3.0f, 0.0f }}, // 8
        {{ 3.0f, 3.0f, 0.0f }}, // 9
        {{ 4.0f, 3.0f, 0.0f }}, // 10
        {{ 5.0f, 3.0f, 0.0f }}, // 11
    };
    const cl_uint expectedIndices[] =
    {
        0, 5, 1,
        1, 5, 2,
        2, 5, 3,
        3, 5, 4,
        6, 8, 7,
        7, 8, 9,
        9, 8, 10,
        9, 10, 11,
        6, 10, 8
    };

    MemoryWriterPly writer;

    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer));
    // There are 22 vertices total, and we want a threshold of 6
    mesher->setPruneThreshold(6.5 / 22.0);
    unsigned int passes = mesher->numPasses();
    for (unsigned int i = 0; i < passes; i++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(i);
        add(ChunkId(), functor,
            boost::size(internalVertices0),
            boost::size(externalVertices0),
            boost::size(indices0),
            internalVertices0, externalVertices0, externalKeys0, indices0);
        add(ChunkId(), functor,
            boost::size(internalVertices1),
            boost::size(externalVertices1),
            boost::size(indices1),
            internalVertices1, externalVertices1, externalKeys1, indices1);
        add(ChunkId(), functor,
            0, boost::size(externalVertices2), boost::size(indices2),
            NULL, externalVertices2, externalKeys2, indices2);
        add(ChunkId(), functor,
            boost::size(internalVertices3),
            boost::size(externalVertices3),
            boost::size(indices3),
            internalVertices3, externalVertices3, externalKeys3, indices3);
    }
    mesher->write(tworker);

    checkIsomorphic(boost::size(expectedVertices), boost::size(expectedIndices),
                    expectedVertices, expectedIndices, writer.getOutput(""));
}

void TestMesherBase::testChunk()
{
    Timeplot::Worker tworker("test");

    const boost::array<cl_float, 3> expectedVertices2[] =
    {
        {{ 0.0f, 1.0f, 0.0f }},
        {{ 0.0f, 2.0f, 0.0f }},
        {{ 0.0f, 3.0f, 0.0f }},
        {{ 2.0f, 0.0f, 1.0f }},
        {{ 2.0f, 0.0f, 2.0f }}
    };

    const boost::array<cl_float, 3> expectedVertices3[] =
    {
        {{ 3.0f, 3.0f, 3.0f }},
        {{ 4.0f, 5.0f, 6.0f }},
        {{ 1.0f, 0.0f, 2.0f }},
        {{ 1.0f, 0.0f, 3.0f }},
        {{ 2.0f, 0.0f, 2.0f }}
    };

    ChunkNamer namer("chunk");
    MemoryWriterPly writer;
    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer, namer));
    unsigned int passes = mesher->numPasses();

    ChunkId chunkId[4];
    for (unsigned int i = 0; i < 4; i++)
    {
        chunkId[i].gen = i;
        chunkId[i].coords[0] = i;
        chunkId[i].coords[1] = i * i;
        chunkId[i].coords[2] = 1;
    }
    for (unsigned int i = 0; i < passes; i++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(i);
        add(chunkId[0], functor,
            boost::size(internalVertices0), 0, boost::size(indices0),
            internalVertices0, NULL, NULL, indices0);
        add(chunkId[1], functor,
            0, boost::size(externalVertices1), boost::size(indices1),
            NULL, externalVertices1, externalKeys1, indices1);
        add(chunkId[2], functor,
            boost::size(internalVertices2),
            boost::size(externalVertices2),
            boost::size(indices2),
            internalVertices2, externalVertices2, externalKeys2, indices2);
        add(chunkId[3], functor,
            boost::size(internalVertices3),
            boost::size(externalVertices3),
            boost::size(indices3),
            internalVertices3, externalVertices3, externalKeys3, indices3);
    }
    mesher->write(tworker);

    checkIsomorphic(boost::size(internalVertices0),
                    boost::size(indices0),
                    internalVertices0, indices0, writer.getOutput("chunk_0000_0000_0001.ply"));
    checkIsomorphic(boost::size(externalVertices1),
                    boost::size(indices1),
                    externalVertices1, indices1, writer.getOutput("chunk_0001_0001_0001.ply"));
    checkIsomorphic(boost::size(expectedVertices2),
                    boost::size(indices2),
                    expectedVertices2, indices2, writer.getOutput("chunk_0002_0004_0001.ply"));
    checkIsomorphic(boost::size(expectedVertices3),
                    boost::size(indices3),
                    expectedVertices3, indices3, writer.getOutput("chunk_0003_0009_0001.ply"));
}

static int simpleRandomInt(std::tr1::mt19937 &engine, int min, int max)
{
    using std::tr1::mt19937;
    using std::tr1::uniform_int;
    using std::tr1::variate_generator;

    /* According to TR1, there has to be a conversion from the output type
     * of the engine to the input type of the distribution, so we can't
     * just use uniform_int<int> (and MSVC will do the wrong thing in this
     * case). We thus also have to manually bias to avoid negative numbers.
     */
    variate_generator<mt19937 &, uniform_int<mt19937::result_type> > gen(engine, uniform_int<mt19937::result_type>(0, max - min));
    return int(gen()) + min;
}

void TestMesherBase::testRandom()
{
    Timeplot::Worker tworker("test");

    /* For this random test we wish to test pruning, chunking, and welding. We
     * start with a number of components, each of which is a rectangular grid
     * subdivided into triangles. The triangles are then randomly assigned to
     * chunks and those in each chunk randomly subdivided into blocks.
     * Vertices are considered to be external if they belong to more than one
     * block.
     *
     * The pruning threshold is set to the average component size, so that
     * hopefully about half the components will be pruned.
     *
     * To simplify the setup, all vertices are given unique keys, and these
     * keys are used as triangle indices in intermediate stages so that they
     * never need to be rewritten as the triangles move around. Only at the
     * end are vertex keys looked up to form vertex indices.
     */

    std::tr1::mt19937 engine;

    const unsigned int numChunks = 5;
    const unsigned int numBlocksPerChunk = 8;
    const unsigned int numBlocks = numChunks * numBlocksPerChunk;
    const unsigned int numComponents = 70;

    std::vector<Component> components(numComponents);
    std::vector<Chunk> chunks(numChunks);
    std::tr1::unordered_map<cl_ulong, Vertex> allVertices;
    for (unsigned int i = 0; i < numChunks; i++)
    {
        chunks[i].id.gen = i;
        chunks[i].id.coords[0] = i;
        chunks[i].blocks.resize(numBlocksPerChunk);
    }
    for (unsigned int cid = 0; cid < numComponents; cid++)
    {
        Component &c = components[cid];
        c.width = simpleRandomInt(engine, 2, 200);
        c.height = simpleRandomInt(engine, 2, 150);
        for (int i = 0; i < c.height; i++)
            for (int j = 0; j < c.width; j++)
            {
                Vertex v;
                v.coords[0] = cid;
                v.coords[1] = i;
                v.coords[2] = j;
                v.owners = 0;

                cl_ulong key = (cl_ulong(cid) << 32) | (i << 16) | j;
                allVertices[key] = v;
                c.vertices.push_back(key);
            }
        for (int i = 0; i + 1 < c.height; i++)
            for (int j = 0; j + 1 < c.width; j++)
            {
                unsigned int base = i * c.width + j;
                boost::array<cl_ulong, 3> triangle;
                triangle[0] = c.vertices[base];
                triangle[1] = c.vertices[base + 1];
                triangle[2] = c.vertices[base + c.width];
                c.triangles.push_back(triangle);
                triangle[0] = triangle[2];
                triangle[2] = c.vertices[base + c.width + 1];
                c.triangles.push_back(triangle);
            }
    }

    const double pruneThreshold = 1.0 / numComponents;
    const std::size_t pruneThresholdVertices = std::size_t(allVertices.size() * pruneThreshold);
    // Assign triangles to blocks and compute expected outputs
    for (unsigned int cid = 0; cid < numComponents; cid++)
    {
        Component &c = components[cid];
        bool retain = c.vertices.size() >= pruneThresholdVertices;

        for (std::size_t i = 0; i < c.triangles.size(); i++)
        {
            int blockNum = simpleRandomInt(engine, 0, numBlocks - 1);
            int chunkNum = blockNum / numBlocksPerChunk;
            int chunkBlockNum = blockNum % numBlocksPerChunk;
            Block &block = chunks[chunkNum].blocks[chunkBlockNum];
            for (unsigned int j = 0; j < 3; j++)
            {
                cl_ulong key = c.triangles[i][j];
                if (block.vertices.insert(key).second) // insert successful
                    allVertices[key].owners++;
            }
            block.triangles.push_back(c.triangles[i]);
            if (retain)
            {
                Chunk &chunk = chunks[chunkNum];
                boost::array<cl_uint, 3> triangle;
                for (unsigned int j = 0; j < 3; j++)
                {
                    cl_ulong key = c.triangles[i][j];
                    std::pair<std::tr1::unordered_map<cl_ulong, std::size_t>::iterator, bool> added;
                    added = chunk.indices.insert(std::make_pair(key, chunk.expectedVertices.size()));
                    if (added.second)
                    {
                        // New vertex for the chunk
                        chunk.expectedVertices.push_back(allVertices[key].coords);
                    }
                    triangle[j] = added.first->second;
                }
                chunk.expectedTriangles.push_back(triangle);
            }
        }
    }

    // Complete the blocks
    for (unsigned int i = 0; i < numChunks; i++)
        for (unsigned int j = 0; j < numBlocksPerChunk; j++)
        {
            Block &block = chunks[i].blocks[j];

            // count internal vertices
            unsigned int internal = 0;
            unsigned int external = 0;
            BOOST_FOREACH(cl_ulong key, block.vertices)
            {
                const Vertex &v = allVertices[key];
                if (v.owners <= 1)
                    internal++;
            }
            MeshSizes sizes(block.vertices.size(), block.triangles.size(), internal);
            block.buffer.reset(new char[sizes.getHostBytes()]);
            block.work.mesh = HostKeyMesh(block.buffer.get(), sizes);

            std::tr1::unordered_map<cl_ulong, std::size_t> indices;
            internal = 0;
            BOOST_FOREACH(cl_ulong key, block.vertices)
            {
                const Vertex &v = allVertices[key];
                if (v.owners > 1)
                {
                    block.work.mesh.vertices[sizes.numInternalVertices() + external] = v.coords;
                    block.work.mesh.vertexKeys[external] = key;
                    indices[key] = sizes.numInternalVertices() + external;
                    external++;
                }
                else
                {
                    block.work.mesh.vertices[internal] = v.coords;
                    indices[key] = internal;
                    internal++;
                }
            }

            for (std::size_t k = 0; k < block.triangles.size(); k++)
                for (unsigned int l = 0; l < 3; l++)
                    block.work.mesh.triangles[k][l] = indices[block.triangles[k][l]];
            block.work.chunkId = chunks[i].id;
        }

    /* Now the actual testing */
    ChunkNamer namer("chunk");
    MemoryWriterPly writer;
    boost::scoped_ptr<MesherBase> mesher(mesherFactory(writer, namer));
    mesher->setPruneThreshold(pruneThreshold);
    unsigned int passes = mesher->numPasses();

    for (unsigned int pass = 0; pass < passes; pass++)
    {
        const MesherBase::InputFunctor functor = mesher->functor(pass);
        BOOST_FOREACH(Chunk &chunk, chunks)
        {
            BOOST_FOREACH(Block &block, chunk.blocks)
            {
                CLH::enqueueMarkerWithWaitList(queue, NULL, &block.work.verticesEvent);
                CLH::enqueueMarkerWithWaitList(queue, NULL, &block.work.vertexKeysEvent);
                CLH::enqueueMarkerWithWaitList(queue, NULL, &block.work.trianglesEvent);
                block.work.hasEvents = true;
                queue.flush();
                functor(block.work, tworker);
            }
        }
    }
    mesher->write(tworker);

    BOOST_FOREACH(Chunk &chunk, chunks)
    {
        const std::string name = namer(chunk.id);
        if (chunk.expectedTriangles.empty())
        {
            CPPUNIT_ASSERT_THROW(writer.getOutput(name), std::invalid_argument);
        }
        else
        {
            checkIsomorphic(chunk.expectedVertices.size(),
                            chunk.expectedTriangles.size() * 3,
                            &chunk.expectedVertices[0],
                            &chunk.expectedTriangles[0][0],
                            writer.getOutput(name));
        }
    }
}

/// Tests from @ref TestMesherBase that are split out before they're slower
class TestMesherBaseSlow : public TestMesherBase
{
    CPPUNIT_TEST_SUITE(TestMesherBaseSlow);
    CPPUNIT_TEST(testRandom);
    CPPUNIT_TEST_SUITE_END_ABSTRACT();
};

class TestOOCMesher : public TestMesherBase
{
    CPPUNIT_TEST_SUB_SUITE(TestOOCMesher, TestMesherBase);
    CPPUNIT_TEST_SUITE_END();
protected:
    virtual MesherBase *mesherFactory(FastPly::Writer &writer, const MesherBase::Namer &namer);
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestOOCMesher, TestSet::perBuild());

/**
 * Tests for OOCMesher::TmpWriterWorkerGroup
 */
class TestTmpWriterWorkerGroup : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE(TestTmpWriterWorkerGroup);
    CPPUNIT_TEST(testInitialState);
    CPPUNIT_TEST(testRandom);
    CPPUNIT_TEST_SUITE_END();

private:
    /// Checks that an obtained item is empty
    void checkEmpty(const OOCMesher::TmpWriterItem &item);

    OOCMesher::TmpWriterWorkerGroup group;

public:
    void testInitialState();  ///< Tests that the paths are initially empty
    void testRandom();        ///< Throws in lots of random data, checks that it comes back

    virtual void tearDown();  ///< Delete the temporary files

    TestTmpWriterWorkerGroup() : group(3) {}
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestTmpWriterWorkerGroup, TestSet::perCommit());

void TestTmpWriterWorkerGroup::checkEmpty(const OOCMesher::TmpWriterItem &item)
{
    CPPUNIT_ASSERT(item.vertices.empty());
    CPPUNIT_ASSERT(item.triangles.empty());
    CPPUNIT_ASSERT(item.vertexRanges.empty());
    CPPUNIT_ASSERT(item.triangleRanges.empty());
}

void TestTmpWriterWorkerGroup::testInitialState()
{
    CPPUNIT_ASSERT(group.getVerticesPath().empty());
    CPPUNIT_ASSERT(group.getTrianglesPath().empty());
}

void TestTmpWriterWorkerGroup::testRandom()
{
    using std::tr1::mt19937;
    using std::tr1::uniform_int;
    using std::tr1::uniform_real;
    using std::tr1::variate_generator;
    typedef OOCMesher::triangle_type triangle_type;
    typedef OOCMesher::vertex_type vertex_type;

    mt19937 engine;
    variate_generator<mt19937 &, uniform_int<mt19937::result_type> > genNum(engine, uniform_int<mt19937::result_type>(0, 50));
    variate_generator<mt19937 &, uniform_int<mt19937::result_type> > genTriangle(engine, uniform_int<mt19937::result_type>(0, 100000000));
    variate_generator<mt19937 &, uniform_real<float> > genVertex(engine, uniform_real<float>(-100.0f, 100.0f));

    Timeplot::Worker tworker("test");
    group.start();

    std::vector<vertex_type> expectedVertices;
    std::vector<triangle_type> expectedTriangles;

    for (int i = 0; i < 100; i++)
    {
        boost::shared_ptr<OOCMesher::TmpWriterItem> item = group.get(tworker, 1);
        checkEmpty(*item);

        int numVertices = genNum();
        int numTriangles = genNum();
        int numVertexRanges = genNum();
        int numTriangleRanges = genNum();
        std::vector<vertex_type> vertices;
        std::vector<triangle_type> triangles;
        for (int j = 0; j < numVertices; j++)
        {
            vertex_type v;
            for (int k = 0; k < 3; k++)
                v[k] = genVertex();
            item->vertices.push_back(v);
            vertices.push_back(v);
        }
        for (int j = 0; j < numTriangles; j++)
        {
            triangle_type t;
            for (int k = 0; k < 3; k++)
                t[k] = genTriangle();
            item->triangles.push_back(t);
            triangles.push_back(t);
        }
        for (int j = 0; j < numVertexRanges; j++)
        {
            std::size_t a = uniform_int<mt19937::result_type>(0, numVertices)(engine);
            std::size_t b = uniform_int<mt19937::result_type>(0, numVertices)(engine);
            if (a > b)
                swap(a, b);
            item->vertexRanges.push_back(std::make_pair(a, b));
            for (std::size_t k = a; k < b; k++)
                expectedVertices.push_back(vertices[k]);
        }
        for (int j = 0; j < numTriangleRanges; j++)
        {
            std::size_t a = uniform_int<mt19937::result_type>(0, numTriangles)(engine);
            std::size_t b = uniform_int<mt19937::result_type>(0, numTriangles)(engine);
            if (a > b)
                swap(a, b);
            item->triangleRanges.push_back(std::make_pair(a, b));
            for (std::size_t k = a; k < b; k++)
                expectedTriangles.push_back(triangles[k]);
        }

        group.push(tworker, item);
    }

    group.stop();

    CPPUNIT_ASSERT(!group.verticesFile.is_open());
    CPPUNIT_ASSERT(!group.trianglesFile.is_open());
    CPPUNIT_ASSERT(!group.getVerticesPath().empty());
    CPPUNIT_ASSERT(!group.getTrianglesPath().empty());

    boost::filesystem::ifstream inVertices(group.getVerticesPath(), std::ios::in | std::ios::binary);
    boost::filesystem::ifstream inTriangles(group.getTrianglesPath(), std::ios::in | std::ios::binary);
    std::vector<vertex_type> actualVertices(expectedVertices.size());
    std::vector<triangle_type> actualTriangles(expectedTriangles.size());

    inVertices.read(reinterpret_cast<char *>(&actualVertices[0]), expectedVertices.size() * sizeof(vertex_type));
    CPPUNIT_ASSERT(inVertices);
    MLSGPU_ASSERT_EQUAL(expectedVertices.size() * sizeof(vertex_type), inVertices.gcount());
    for (std::size_t i = 0; i < expectedVertices.size(); i++)
        for (int j = 0; j < 3; j++)
            CPPUNIT_ASSERT_EQUAL(expectedVertices[i][j], actualVertices[i][j]);

    inTriangles.read(reinterpret_cast<char *>(&actualTriangles[0]), expectedTriangles.size() * sizeof(triangle_type));
    CPPUNIT_ASSERT(inTriangles);
    MLSGPU_ASSERT_EQUAL(expectedTriangles.size() * sizeof(triangle_type), inTriangles.gcount());
    for (std::size_t i = 0; i < expectedTriangles.size(); i++)
        for (int j = 0; j < 3; j++)
            CPPUNIT_ASSERT_EQUAL(expectedTriangles[i][j], actualTriangles[i][j]);
}

void TestTmpWriterWorkerGroup::tearDown()
{
    if (group.running())
        group.stop();
    if (!group.getVerticesPath().empty())
        boost::filesystem::remove(group.getVerticesPath());
    if (!group.getTrianglesPath().empty())
        boost::filesystem::remove(group.getTrianglesPath());
}

class TestOOCMesherSlow : public TestOOCMesher
{
    CPPUNIT_TEST_SUB_SUITE(TestOOCMesherSlow, TestMesherBaseSlow);
    CPPUNIT_TEST_SUITE_END();
};
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(TestOOCMesherSlow, TestSet::perCommit());

MesherBase *TestOOCMesher::mesherFactory(FastPly::Writer &writer, const MesherBase::Namer &namer)
{
    return new OOCMesher(writer, namer);
}

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
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <boost/program_options.hpp>
#include <boost/foreach.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/scoped_array.hpp>
#include <boost/thread/thread.hpp>
#include <boost/progress.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/bind.hpp>
#include "src/tr1_unordered_map.h"
#include <iostream>
#include <map>
#include <vector>
#include <algorithm>
#include <numeric>
#include <mpi.h>
#include "src/misc.h"
#include "src/clh.h"
#include "src/logging.h"
#include "src/timer.h"
#include "src/fast_ply.h"
#include "src/splat.h"
#include "src/grid.h"
#include "src/splat_tree_cl.h"
#include "src/marching.h"
#include "src/mls.h"
#include "src/mesher.h"
#include "src/mesher_mpi.h"
#include "src/options.h"
#include "src/splat_set_mpi.h"
#include "src/bucket.h"
#include "src/provenance.h"
#include "src/statistics.h"
#include "src/statistics_cl.h"
#include "src/work_queue.h"
#include "src/circular_buffer.h"
#include "src/workers.h"
#include "src/progress.h"
#include "src/progress_mpi.h"
#include "src/mesh_filter.h"
#include "src/timeplot.h"
#include "src/bucket_loader.h"
#include "src/bucket_collector.h"
#include "src/worker_group_mpi.h"
#include "src/serialize.h"
#include "src/mlsgpu_core.h"

namespace po = boost::program_options;
using namespace std;

template<>
void sendItem(const MesherGroup::WorkItem &item, MPI_Comm comm, int dest)
{
    Serialize::send(item.work, comm, dest);
}

template<>
void recvItem(MesherGroup::WorkItem &item, MPI_Comm comm, int dest)
{
    Serialize::recv(item.work, item.alloc.get(), comm, dest);
}

template<>
std::size_t sizeItem(const MesherGroup::WorkItem &item)
{
    return item.work.mesh.getHostBytes();
}

typedef SplatSet::FastBlobSetMPI<SplatSet::FileSet> Splats;

namespace
{

/**
 * Function object for doing the GPU work. There is one slave launched
 * on each node that has GPUs.
 */
class Slave
{
private:
    const std::vector<std::pair<cl::Context, cl::Device> > &devices;
    const po::variables_map &vm;
    Splats &splats;
    MPI_Comm scatterComm;
    int scatterRoot;
    MPI_Comm gatherComm;
    int gatherRoot;
    MPI_Comm progressComm;
    int progressRoot;

public:
    Slave(const std::vector<std::pair<cl::Context, cl::Device> > &devices,
          const po::variables_map &vm,
          Splats &splats,
          MPI_Comm scatterComm, int scatterRoot,
          MPI_Comm gatherComm, int gatherRoot,
          MPI_Comm progressComm, int progressRoot)
        : devices(devices), vm(vm), splats(splats),
        scatterComm(scatterComm), scatterRoot(scatterRoot),
        gatherComm(gatherComm), gatherRoot(gatherRoot),
        progressComm(progressComm), progressRoot(progressRoot)
    {
    }

    void operator()() const;
};

/**
 * Receives collections of bins from @ref BucketCollector and passes them over MPI.
 */
class Scatter
{
private:
    MPI_Comm comm;
    Timeplot::Worker &tworker;

    Statistics::Variable &waitStat;
    Statistics::Variable &sendStat;
public:
    typedef void result_type;

    /// Constructor
    Scatter(MPI_Comm comm, Timeplot::Worker &tworker);

    /// Send the bins to a slave
    void operator()(const Statistics::Container::vector<BucketCollector::Bin> &bins) const;

    /// Shuts down the slaves
    void stop(std::size_t numSlaves) const;
};

class GatherGroup : public WorkerGroupGather<MesherGroup::WorkItem, GatherGroup>
{
public:
    typedef MesherGroup::WorkItem WorkItem;

    GatherGroup(MPI_Comm comm, int root, std::size_t bufferSize)
        : WorkerGroupGather<WorkItem, GatherGroup>("gather", comm, root),
        meshBuffer("mem.GatherGroup.mesh", bufferSize)
    {
    }

    boost::shared_ptr<WorkItem> get(Timeplot::Worker &tworker, std::size_t size)
    {
        boost::shared_ptr<WorkItem> item = WorkerGroupGather<WorkItem, GatherGroup>::get(tworker, size);
        std::size_t rounded = roundUp(size, sizeof(cl_ulong)); // to ensure alignment
        item->alloc = meshBuffer.allocate(tworker, rounded, &getStat);
        return item;
    }

    void freeItem(boost::shared_ptr<WorkItem> item)
    {
        meshBuffer.free(item->alloc);
    }

private:
    CircularBuffer meshBuffer;
};

Scatter::Scatter(MPI_Comm comm, Timeplot::Worker &tworker) :
    comm(comm),
    tworker(tworker),
    waitStat(Statistics::getStatistic<Statistics::Variable>("scatter.get")),
    sendStat(Statistics::getStatistic<Statistics::Variable>("scatter.push"))
{
}

void Scatter::operator()(const Statistics::Container::vector<BucketCollector::Bin> &bins) const
{
    if (bins.empty())
        return;

    int needsWork;
    MPI_Status status;
    {
        Timeplot::Action timer("wait", tworker, waitStat);
        MPI_Recv(&needsWork, 1, MPI_INT, MPI_ANY_SOURCE, MLSGPU_TAG_SCATTER_NEED_WORK, comm, &status);
    }

    {
        Timeplot::Action timer("send", tworker, sendStat);
        int dest = status.MPI_SOURCE;
        std::size_t workSize = bins.size();
        MPI_Send(&workSize, 1, Serialize::mpi_type_traits<std::size_t>::type(),
                 dest, MLSGPU_TAG_SCATTER_HAS_WORK, comm);
        for (std::size_t i = 0; i < bins.size(); i++)
        {
            Serialize::send(bins[i], comm, dest);
        }
    }
}

void Scatter::stop(std::size_t numSlaves) const
{
    for (std::size_t i = 0; i < numSlaves; i++)
    {
        int needsWork;
        MPI_Status status;
        {
            Timeplot::Action timer("wait", tworker, waitStat);
            MPI_Recv(&needsWork, 1, MPI_INT, MPI_ANY_SOURCE, MLSGPU_TAG_SCATTER_NEED_WORK, comm, &status);
        }

        {
            Timeplot::Action timer("send", tworker, sendStat);
            int dest = status.MPI_SOURCE;
            std::size_t workSize = 0; // signals shutdown
            MPI_Send(&workSize, 1, Serialize::mpi_type_traits<std::size_t>::type(),
                     dest, MLSGPU_TAG_SCATTER_HAS_WORK, comm);
        }
    }
}

void Slave::operator()() const
{
    Timeplot::Worker tworker("slave");
    Statistics::Variable &firstPopStat = Statistics::getStatistic<Statistics::Variable>("slave.pop.first");
    Statistics::Variable &popStat = Statistics::getStatistic<Statistics::Variable>("slave.pop");
    Statistics::Variable &recvStat = Statistics::getStatistic<Statistics::Variable>("slave.recv");

    const std::size_t memGather = vm[Option::memGather].as<Capacity>();

    GatherGroup gatherGroup(gatherComm, gatherRoot, memGather);
    SlaveWorkers slaveWorkers(tworker, vm, devices, makeOutputGenerator(gatherGroup));

    /* NB: this does not yet support multi-pass algorithms. Currently there
     * are none, however.
     */

    ProgressMPI progress(NULL, splats.numSplats(), progressComm, progressRoot);
    slaveWorkers.start(splats, splats.getBoundingGrid(), &progress);
    gatherGroup.start();

    bool first = true;
    while (true)
    {
        int needWork = 1;
        std::size_t workSize;
        {
            Timeplot::Action timer("pop", tworker, first ? firstPopStat : popStat);
            MPI_Sendrecv(&needWork, 1, MPI_INT, scatterRoot, MLSGPU_TAG_SCATTER_NEED_WORK,
                         &workSize, 1, Serialize::mpi_type_traits<std::size_t>::type(), scatterRoot, MLSGPU_TAG_SCATTER_HAS_WORK,
                         scatterComm, MPI_STATUS_IGNORE);
            if (workSize == 0)
                break;
        }

        Statistics::Container::vector<BucketCollector::Bin> bins("mem.BucketCollector.bins", workSize);
        {
            Timeplot::Action timer("recv", tworker, recvStat);
            for (std::size_t i = 0; i < bins.size(); i++)
                Serialize::recv(bins[i], scatterComm, scatterRoot);
        }
        (*slaveWorkers.loader)(bins);
    }

    slaveWorkers.stop();
    gatherGroup.stop();
    progress.sync();

    Statistics::finalizeEventTimes();
}

/**
 * Collect statistics from all rank, and write to the output file on the @a root
 * rank.
 */
static void doStatistics(const po::variables_map &vm, MPI_Comm comm, int root)
{
    int rank;
    int size;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);

    if (rank == root)
    {
        for (int slave = 0; slave < size; slave++)
            if (slave != root)
            {
                MPI_Status status;
                MPI_Probe(MPI_ANY_SOURCE, MLSGPU_TAG_WORK, comm, &status);
                int length;
                MPI_Get_count(&status, MPI_CHAR, &length);
                boost::scoped_array<char> data(new char[length]);
                MPI_Recv(data.get(), length, MPI_CHAR, status.MPI_SOURCE, MLSGPU_TAG_WORK, comm, MPI_STATUS_IGNORE);

                std::string statsStr(data.get(), length);
                std::istringstream statsStream(statsStr);
                boost::archive::text_iarchive ia(statsStream);
                Statistics::Registry slaveRegistry;
                ia >> slaveRegistry;
                Statistics::Registry::getInstance().merge(slaveRegistry);
            }
        writeStatistics(vm);
    }
    else
    {
        std::ostringstream statsStream;
        boost::archive::text_oarchive oa(statsStream);
        oa << Statistics::Registry::getInstance();
        std::string statsStr = statsStream.str();
        MPI_Send(const_cast<char *>(statsStr.data()), statsStr.length(), MPI_CHAR,
                 root, MLSGPU_TAG_WORK, comm);
    }
}

/**
 * Execution in @c --resume mode
 *
 * @param comm            Communicator indicating the group to run on
 * @param out             Output filename or basename
 * @param vm              Command-line options
 * @return Number of output files written
 */
static std::size_t runResume(
    MPI_Comm comm, const std::string &out, const po::variables_map &vm)
{
    const int root = 0;
    int rank;
    MPI_Comm_rank(comm, &rank);
    std::size_t ret = 0;

    {
        Timeplot::Worker mainWorker("main");
        Statistics::Timer grandTotalTimer("run.time");

        boost::scoped_ptr<FastPly::WriterMPI> writer(new FastPly::WriterMPI);
        setWriterComments(vm, *writer);
        boost::scoped_ptr<MesherBase> mesher(new OOCMesherMPI(*writer, getNamer(vm, out), comm, root));
        setMesherOptions(vm, *mesher);

        boost::filesystem::path path(vm[Option::resume].as<std::string>());
        ret = mesher->resume(mainWorker, path, &Log::log[Log::info]);
    }

    doStatistics(vm, comm, root);
    return ret;
}

/**
 * Main execution.
 *
 * @param comm            Communicator indicating the group to run on
 * @param devices         List of OpenCL devices to use
 * @param out             Output filename or basename
 * @param vm              Command-line options
 * @return Number of output files written
 */
static std::size_t run(
    MPI_Comm comm,
    const std::vector<std::pair<cl::Context, cl::Device> > &devices,
    const string &out,
    const po::variables_map &vm)
{
    const int root = 0;
    int rank, size;
    MPI_Comm scatterComm;
    MPI_Comm gatherComm;
    MPI_Comm progressComm;

    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &size);
    MPI_Comm_dup(comm, &scatterComm);
    MPI_Comm_dup(comm, &gatherComm);
    MPI_Comm_dup(comm, &progressComm);

    Timeplot::Worker mainWorker("main");
    boost::scoped_ptr<Statistics::Timer> grandTotalTimer;
    if (rank == root)
        grandTotalTimer.reset(new Statistics::Timer("run.time"));

    /* Work out how many slaves there will be */
    int isSlave = devices.empty() ? 0 : 1;
    vector<int> slaveMask(size);
    MPI_Gather(&isSlave, 1, MPI_INT, &slaveMask[0], 1, MPI_INT, root, comm);

    Splats splats;
    doComputeBlobs(mainWorker, vm, splats,
                   boost::bind(&SplatSet::FastBlobSetMPI<SplatSet::FileSet>::computeBlobs,
                               &splats, comm, root, _1, _2, &Log::log[Log::info], true));

    boost::scoped_ptr<boost::thread> slaveThread;
    if (!devices.empty())
    {
        slaveThread.reset(new boost::thread(Slave(
                    devices, vm, splats,
                    scatterComm, root, gatherComm, root,
                    progressComm, root)));
    }

    boost::scoped_ptr<FastPly::WriterMPI> writer(new FastPly::WriterMPI);
    setWriterComments(vm, *writer);
    boost::scoped_ptr<MesherBase> mesher(new OOCMesherMPI(*writer, getNamer(vm, out), comm, root));
    setMesherOptions(vm, *mesher);

    if (rank == root)
    {
        const int numSlaves = accumulate(slaveMask.begin(), slaveMask.end(), 0);
        const std::size_t maxLoadSplats = getMaxLoadSplats(vm);
        const std::size_t memMesh = vm[Option::memMesh].as<Capacity>();

        const Grid grid = splats.getBoundingGrid();
        const unsigned int chunkCells = postprocessGrid(vm, grid);

        {
            // Open a scope so that objects will be released before finalization
            boost::scoped_ptr<Timeplot::Action> initTimer(new Timeplot::Action("init", mainWorker, "init.time"));

            MesherGroup mesherGroup(memMesh);
            ReceiverGather<MesherGroup::WorkItem, MesherGroup> receiver("receiver", mesherGroup, gatherComm, numSlaves);
            Scatter scatter(scatterComm, mainWorker);
            BucketCollector collector(maxLoadSplats, scatter);

            initTimer.reset();

            for (unsigned int pass = 0; pass < mesher->numPasses(); pass++)
            {
                Log::log[Log::info] << "\nPass " << pass + 1 << "/" << mesher->numPasses() << endl;
                ostringstream passName;
                passName << "pass" << pass + 1 << ".time";
                Statistics::Timer timer(passName.str());

                ProgressDisplay progress(splats.numSplats(), Log::log[Log::info]);
                ProgressMPI progressMPI(&progress, splats.numSplats(), progressComm, 0);

                mesherGroup.setInputFunctor(mesher->functor(pass));

                // Start threads
                boost::thread receiverThread(boost::ref(receiver));
                mesherGroup.start();
                boost::thread progressThread(boost::ref(progressMPI));

                try
                {
                    doBucket(mainWorker, vm, splats, grid, chunkCells, collector);
                }
                catch (...)
                {
                    // This can't be handled using unwinding, because that would operate in
                    // the wrong order
                    collector.flush();
                    scatter.stop(numSlaves);
                    receiverThread.join();
                    mesherGroup.stop();
                    progressMPI.sync();
                    progressThread.interrupt();
                    progressThread.join();
                    throw;
                }

                /* Shut down threads. Note that it has to be done in forward order to
                 * satisfy the requirement that stop() is only called after producers
                 * are terminated.
                 */
                collector.flush();
                scatter.stop(numSlaves);
                receiverThread.join();
                mesherGroup.stop();
                progressMPI.sync();
                progressThread.join();
            }
        }
    }
    if (slaveThread)
        slaveThread->join();

    std::size_t ret = mesher->write(mainWorker, &Log::log[Log::info]);

    grandTotalTimer.reset();
    doStatistics(vm, comm, root);
    return ret;
}

} // anonymous namespace

int main(int argc, char **argv)
{
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE)
    {
        std::cerr << "MPI implementation does not provide the required level of thread support\n";
        MPI_Finalize();
        return 1;
    }

    Serialize::init();

    Log::log.setLevel(Log::info);
    po::variables_map vm = processOptions(argc, argv, true);
    setLogLevel(vm);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    std::vector<cl::Device> devices = CLH::findDevices(vm);
    int numDevices = devices.size();
    int totalDevices;
    MPI_Reduce(&numDevices, &totalDevices, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

    if (rank == 0)
    {
        if (totalDevices == 0)
        {
            cerr << "No suitable OpenCL device found\n";
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        try
        {
            validateOptions(vm, true);
        }
        catch (invalid_option &e)
        {
            cerr << e.what() << endl;
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }
    CLH::ResourceUsage totalUsage = resourceUsage(vm);

    if (rank == 0)
        Log::log[Log::info] << "About " << totalUsage.getTotalMemory() / (1024 * 1024) << "MiB of device memory will be used per device.\n";

    /* Give each node a turn to validate things. Doing it serially prevents
     * the output from becoming interleaved.
     */
    for (int node = 0; node < size; node++)
    {
        if (node == rank)
        {
            BOOST_FOREACH(const cl::Device &device, devices)
            {
                try
                {
                    validateDevice(device, totalUsage);
                }
                catch (CLH::invalid_device &e)
                {
                    cerr << e.what() << endl;
                    MPI_Abort(MPI_COMM_WORLD, 1);
                }
                Log::log[Log::info] << "Using device " << device.getInfo<CL_DEVICE_NAME>() << "\n";
            }
        }
        MPI_Barrier(MPI_COMM_WORLD);
    }

    std::vector<std::pair<cl::Context, cl::Device> > cd;
    cd.reserve(devices.size());
    for (std::size_t i = 0; i < devices.size(); i++)
    {
        cd.push_back(std::make_pair(CLH::makeContext(devices[i]), devices[i]));
    }

    try
    {
        if (vm.count(Option::timeplot))
        {
            ostringstream name;
            name << vm[Option::timeplot].as<string>() << "." << rank;
            Timeplot::init(name.str());
        }

        std::size_t filesWritten;
        if (vm.count(Option::resume))
            filesWritten = runResume(MPI_COMM_WORLD, vm[Option::outputFile].as<string>(), vm);
        else
            filesWritten = run(MPI_COMM_WORLD, cd, vm[Option::outputFile].as<string>(), vm);

        if (rank == 0)
        {
            if (filesWritten == 0)
                Log::log[Log::warn] << "Warning: no output files written!\n";
            else if (filesWritten == 1)
                Log::log[Log::info] << "1 output file written.\n";
            else
                Log::log[Log::info] << filesWritten << " output files written.\n";
        }
    }
    catch (cl::Error &e)
    {
        cerr << "\nOpenCL error in " << e.what() << " (" << e.err() << ")\n";
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    catch (std::ios::failure &e)
    {
        reportException(e);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    catch (std::runtime_error &e)
    {
        reportException(e);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    MPI_Finalize();
    return 0;
}

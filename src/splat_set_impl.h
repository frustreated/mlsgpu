/**
 * @file
 *
 * Implementations of template members from @ref splat_set.h.
 */

#ifndef SPLAT_SET_IMPL_H
#define SPLAT_SET_IMPL_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#if HAVE_OMP_H
# include <omp.h>
#else
# ifndef omp_get_num_threads
#  define omp_get_num_threads() (1)
# endif
# ifndef omp_get_thread_num
#  define omp_get_thread_num() (0)
# endif
#endif
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include "splat_set.h"

namespace SplatSet
{

namespace detail
{

template<typename RangeIterator>
void SimpleVectorSet::MySplatStream<RangeIterator>::refill()
{
    if (curRange != lastRange)
    {
        while (true)
        {
            splat_id end = curRange->second;
            if (owner.size() < end)
                end = owner.size();
            while (cur < end && !owner[cur].isFinite())
                cur++;
            if (cur < end)
                return;

            ++curRange;
            if (curRange != lastRange)
                cur = curRange->first;
            else
                return;
        }
    }
}

template<typename RangeIterator>
SimpleFileSet::ReaderThread<RangeIterator>::ReaderThread(const SimpleFileSet &owner, RangeIterator firstRange, RangeIterator lastRange)
    : SimpleFileSet::ReaderThreadBase(owner), firstRange(firstRange), lastRange(lastRange)
{
}

template<typename RangeIterator>
void SimpleFileSet::ReaderThread<RangeIterator>::operator()()
{
    for (RangeIterator r = firstRange; r != lastRange; ++r)
    {
        splat_id first = r->first;
        splat_id last = r->second;

        boost::scoped_ptr<FastPly::ReaderBase::Handle> handle;
        std::size_t handleId;
        while (first < last)
        {
            std::size_t fileId = first >> scanIdShift;
            if (fileId >= owner.files.size())
                break;

            FastPly::ReaderBase::size_type maxSplats = BUFFER_SIZE / owner.files[fileId].getVertexSize();
            if (maxSplats == 0)
            {
                throw std::runtime_error("Far too many bytes per vertex");
            }
            FastPly::ReaderBase::size_type fileSize = owner.files[fileId].size();
            FastPly::ReaderBase::size_type start = first & splatIdMask;
            FastPly::ReaderBase::size_type end = std::min(start + maxSplats, fileSize);
            if ((last >> scanIdShift) == fileId)
                end = std::min(end, FastPly::ReaderBase::size_type(last & splatIdMask));

            if (start < end)
            {
                if (!handle || handleId != fileId)
                {
                    handle.reset(); // close the old handle first
                    handle.reset(owner.files[fileId].createHandle());
                    handleId = fileId;
                }

                boost::shared_ptr<Item> item = pool.pop();
                item->first = first;
                item->last = first + (end - start);
                item->nSplats = end - start;
                handle->readRaw(start, end, &item->buffer[0]);
                outQueue.push(item);

                first += end - start;
            }
            if (end == fileSize)
            {
                first = (fileId + 1) << scanIdShift;
            }
        }
        // Signal completion
        outQueue.push(boost::shared_ptr<Item>());
    }
}

} // namespace detail

template<typename Base, typename BlobVector>
BlobInfo FastBlobSet<Base, BlobVector>::MyBlobStream::operator*() const
{
    BlobInfo ans;
    MLSGPU_ASSERT(curBlob < lastBlob, std::out_of_range);
    BlobData data = owner.blobs[curBlob];
    ans.firstSplat = data.firstSplat;
    ans.lastSplat = data.lastSplat;
    for (unsigned int i = 0; i < 3; i++)
        ans.lower[i] = divDown(data.lower[i] - offset[i], bucketRatio);
    for (unsigned int i = 0; i < 3; i++)
        ans.upper[i] = divDown(data.upper[i] - offset[i], bucketRatio);
    return ans;
}

template<typename Base, typename BlobVector>
FastBlobSet<Base, BlobVector>::MyBlobStream::MyBlobStream(
    const FastBlobSet<Base, BlobVector> &owner, const Grid &grid,
    Grid::size_type bucketSize)
: owner(owner)
{
    MLSGPU_ASSERT(bucketSize > 0 && owner.internalBucketSize > 0
                  && bucketSize % owner.internalBucketSize == 0, std::invalid_argument);
    for (unsigned int i = 0; i < 3; i++)
        offset[i] = grid.getExtent(i).first / Grid::difference_type(owner.internalBucketSize);
    bucketRatio = bucketSize / owner.internalBucketSize;
    curBlob = 0;
    lastBlob = owner.blobs.size();
}

template<typename Base, typename BlobVector>
BlobStream *FastBlobSet<Base, BlobVector>::makeBlobStream(
    const Grid &grid, Grid::size_type bucketSize) const
{
    if (fastPath(grid, bucketSize))
        return new MyBlobStream(*this, grid, bucketSize);
    else
        return Base::makeBlobStream(grid, bucketSize);
}

namespace detail
{

struct Bbox
{
    boost::array<float, 3> bboxMin, bboxMax;

    Bbox()
    {
        std::fill(bboxMin.begin(), bboxMin.end(), std::numeric_limits<float>::infinity());
        std::fill(bboxMax.begin(), bboxMax.end(), -std::numeric_limits<float>::infinity());
    }

    Bbox &operator+=(const Bbox &b)
    {
        for (int j = 0; j < 3; j++)
        {
            bboxMin[j] = std::min(bboxMin[j], b.bboxMin[j]);
            bboxMax[j] = std::max(bboxMax[j], b.bboxMax[j]);
        }
        return *this;
    }

    Bbox &operator+=(const Splat &splat)
    {
        for (int j = 0; j < 3; j++)
        {
            bboxMin[j] = std::min(bboxMin[j], splat.position[j] - splat.radius);
            bboxMax[j] = std::max(bboxMax[j], splat.position[j] + splat.radius);
        }
        return *this;
    }
};

}

template<typename Base, typename BlobVector>
void FastBlobSet<Base, BlobVector>::computeBlobs(
    float spacing, Grid::size_type bucketSize, std::ostream *progressStream, bool warnNonFinite)
{
    const float ref[3] = {0.0f, 0.0f, 0.0f};

    MLSGPU_ASSERT(bucketSize > 0, std::invalid_argument);
    Statistics::Registry &registry = Statistics::Registry::getInstance();

    blobs.clear();
    internalBucketSize = bucketSize;

    // Reference point will be 0,0,0. Extents are set after reading all the splats
    boundingGrid.setSpacing(spacing);
    boundingGrid.setReference(ref);

    boost::scoped_ptr<ProgressDisplay> progress;
    if (progressStream != NULL)
    {
        *progressStream << "Computing bounding box\n";
        progress.reset(new ProgressDisplay(Base::maxSplats(), *progressStream));
    }

    detail::Bbox bbox;

    boost::scoped_ptr<SplatStream> splats(Base::makeSplatStream());
    nSplats = 0;

    static const std::size_t BUFFER_SIZE = 1024 * 1024;
    Statistics::Container::vector<std::pair<Splat, BlobData> > buffer("mem.computeBlobs.buffer", BUFFER_SIZE);
    while (!splats->empty())
    {
        std::size_t nBuffer = 0;
        do
        {
            buffer[nBuffer].first = **splats;
            buffer[nBuffer].second.firstSplat = splats->currentId();
            nBuffer++;
            ++*splats;
        }
        while (nBuffer < BUFFER_SIZE && !splats->empty());

        /* OpenMP doesn't allow for custom reduce operations, so we have to
         * do it manually by having a separate bbox location for each thread.
         */
        std::vector<detail::Bbox> bboxes;
#pragma omp parallel shared(bboxes, buffer, nBuffer)
        {
#pragma omp master
            {
                bboxes.resize(omp_get_num_threads());
            }
#pragma omp barrier
            int tid = omp_get_thread_num();
#pragma omp for schedule(dynamic, 8192)
            for (std::size_t i = 0; i < nBuffer; i++)
            {
                const Splat &splat = buffer[i].first;
                BlobData &blob = buffer[i].second;
                detail::splatToBuckets(splat, boundingGrid, bucketSize, blob.lower, blob.upper);
                blob.lastSplat = blob.firstSplat + 1;
                bboxes[tid] += buffer[i].first;
            }

#pragma omp master
            {
                for (int i = 0; i < omp_get_num_threads(); i++)
                    bbox += bboxes[i];
            }
        }

        for (std::size_t i = 0; i < nBuffer; i++)
        {
            const BlobData &blob = buffer[i].second;
            if (blobs.empty()
                || blobs.back().lower != blob.lower
                || blobs.back().upper != blob.upper
                || blobs.back().lastSplat != blob.firstSplat)
            {
                blobs.push_back(blob);
            }
            else
            {
                blobs.back().lastSplat++;
            }

        }

        nSplats += nBuffer;
        if (progress != NULL)
            *progress += nBuffer;

    }

    assert(nSplats <= Base::maxSplats());
    splat_id nonFinite = Base::maxSplats() - nSplats;
    if (nonFinite > 0)
    {
        if (progress != NULL)
            *progress += nonFinite;
        if (warnNonFinite)
            Log::log[Log::warn] << "Input contains " << nonFinite << " splat(s) with non-finite values\n";
    }
    registry.getStatistic<Statistics::Variable>("blobset.nonfinite").add(nonFinite);

    if (bbox.bboxMin[0] > bbox.bboxMax[0])
        throw std::runtime_error("Must be at least one splat");

    for (unsigned int i = 0; i < 3; i++)
    {
        float l = bbox.bboxMin[i] / spacing;
        float h = bbox.bboxMax[i] / spacing;
        Grid::difference_type lo = Grid::RoundDown::convert(l);
        Grid::difference_type hi = Grid::RoundUp::convert(h);
        /* The lower extent must be a multiple of the bucket size, to
         * make the blob data align properly.
         */
        lo = divDown(lo, bucketSize) * bucketSize;
        assert(lo % Grid::difference_type(bucketSize) == 0);

        boundingGrid.setExtent(i, lo, hi);
    }
    registry.getStatistic<Statistics::Variable>("blobset.blobs").add(blobs.size());
}

template<typename Base, typename BlobVector>
bool FastBlobSet<Base, BlobVector>::fastPath(const Grid &grid, Grid::size_type bucketSize) const
{
    MLSGPU_ASSERT(internalBucketSize > 0, state_error);
    MLSGPU_ASSERT(bucketSize > 0, std::invalid_argument);
    if (bucketSize % internalBucketSize != 0)
        return false;
    if (boundingGrid.getSpacing() != grid.getSpacing())
        return false;
    for (unsigned int i = 0; i < 3; i++)
    {
        if (grid.getReference()[i] != 0.0f
            || grid.getExtent(i).first % Grid::difference_type(internalBucketSize) != 0)
            return false;
    }
    return true;
}


template<typename Super>
BlobStream *Subset<Super>::makeBlobStream(const Grid &grid, Grid::size_type bucketSize) const
{
    MLSGPU_ASSERT(bucketSize > 0, std::invalid_argument);
    return new detail::SimpleBlobStream(makeSplatStream(), grid, bucketSize);
}

} // namespace SplatSet

#endif /* !SPLAT_SET_IMPL_H */

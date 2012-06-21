/**
 * @file
 *
 * Thread-safe circular buffer for pipelining variable-sized data chunks.
 */

#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <cstddef>
#include <utility>
#include <string>
#include <boost/noncopyable.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/condition_variable.hpp>
#include "tr1_cstdint.h"
#include "allocator.h"

/**
 * Thread-safe circular buffer for pipelining variable-sized data chunks.
 *
 * This buffer is thread-safe when used with one thread that allocates and
 * one thread that frees. It is @em not safe for multi-producer or
 * multi-consumer use, because memory must be freed in the same order it is
 * allocated.
 */
class CircularBuffer : public boost::noncopyable
{
private:
    /**
     * Mutex that protects @ref bufferHead and @ref bufferTail.
     */
    boost::mutex mutex;

    /**
     * Condition signaled whenever space is freed in the buffer.
     */
    boost::condition_variable spaceCondition;

    /**
     * Circular buffer for passing splats to the stream thread. The range
     * [@ref bufferHead, @ref bufferTail) potentially contains data, while the rest
     * is empty. The occupied range may wrap around. If @ref bufferHead ==
     * @ref bufferTail then the buffer is empty; thus it can never be completely full.
     */
    Statistics::Container::vector<char> buffer;
    std::size_t bufferHead;    ///< First data element in @ref buffer
    std::size_t bufferTail;    ///< First empty element in @ref buffer

public:
    /**
     * Allocate some memory from the buffer. It will return as many elements
     * as it can, up to @a max. If there is insufficient memory for a single
     * element, it will block until enough is freed up.
     *
     * To facilitate efficient pipelining, at most half the buffer
     * will be returned at any time.
     *
     * It is thread-safe to call this function at the same time as @a free.
     *
     * @param elementSize     Size of a single element.
     * @param maxElements     Maximum number of elements to allocate.
     * @return A pointer to the allocated data and the number of elements allocated.
     *
     * @pre
     * - @a min &lt;= @ref size() / 2
     * - @a maxElements > 0
     */
    std::pair<void *, std::size_t> allocate(std::size_t elementSize, std::tr1::uintmax_t maxElements);

    /**
     * Free memory allocated by @ref allocate. Each call to @ref allocate must be matched with
     * one to this function, and frees must be performed in the same order they are returned
     * from allocate.
     */
    void free(void *ptr, std::size_t elementSize, std::size_t numElements);

    /**
     * Free memory allocated by @ref allocate. This is an overload that takes
     * the size to free as a single byte count instead of element size and
     * element count.
     */
    void free(void *ptr, std::size_t bytes);

    /// Returns the number of bytes allocated to the buffer.
    std::size_t size() const { return buffer.size(); }

    /**
     * Constructor.
     *
     * @param name      Buffer name used for memory statistic.
     * @param size      Bytes of storage to reserve.
     *
     * @pre @a size &gt;= 2
     */
    CircularBuffer(const std::string &name, std::size_t size);
};

#endif /* !CIRCULAR_BUFFER_H */

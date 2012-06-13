/**
 * @file
 *
 * Classes for gathering and reporting statistics.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <string>
#include <stdexcept>
#include <cmath>
#include <vector>
#include <utility>
#include <queue>
#include <boost/foreach.hpp>
#include <boost/ref.hpp>
#include <boost/thread/locks.hpp>
#include <CL/cl.hpp>
#include "statistics.h"
#include "logging.h"

namespace Statistics
{

Statistic::Statistic(const std::string &name) : name(name)
{
}

Statistic::~Statistic()
{
}

const std::string &Statistic::getName() const
{
    return name;
}

std::ostream &operator<<(std::ostream &o, const Statistic &stat)
{
    boost::lock_guard<boost::mutex> _(stat.mutex);
    o << stat.getName() << ": ";
    stat.write(o);
    return o;
}


Counter::Counter(const std::string &name) : Statistic(name), total(0)
{
}

void Counter::write(std::ostream &o) const
{
    o << total;
}

void Counter::add(unsigned long long incr)
{
    boost::lock_guard<boost::mutex> _(mutex);
    total += incr;
}

unsigned long long Counter::getTotal() const
{
    return total;
}

Variable::Variable(const std::string &name) : Statistic(name), sum(0.0), sum2(0.0), n(0)
{
}

void Variable::add(double value)
{
    boost::lock_guard<boost::mutex> _(mutex);
    sum += value;
    sum2 += value * value;
    n++;
}

unsigned long long Variable::getNumSamples() const
{
    boost::lock_guard<boost::mutex> _(mutex);
    return n;
}

double Variable::getMean() const
{
    boost::lock_guard<boost::mutex> _(mutex);
    if (n < 1)
        throw std::length_error("Cannot compute mean without at least 1 sample");
    return sum / n;
}

double Variable::getStddev() const
{
    return std::sqrt(getVariance());
}

double Variable::getVarianceUnlocked() const
{
    if (n < 2)
        throw std::length_error("Cannot compute variance without at least 2 samples");
    // Theoretically the variable must be non-negative, but rounding errors
    // could make it negative, leading to problems when computing stddev.
    return std::max((sum2 - sum * sum / n) / (n - 1), 0.0);
}

double Variable::getVariance() const
{
    boost::lock_guard<boost::mutex> _(mutex);
    return getVarianceUnlocked();
}

void Variable::write(std::ostream &o) const
{
    if (n >= 1)
        o << sum << " : " << sum / n << ' ';
    if (n >= 2)
        o << "+/- " << std::sqrt(getVarianceUnlocked()) << ' ';
    o << "[" << n << "]";
}

Timer::Timer(const std::string &name)
    : stat(getStatistic<Variable>(name))
{
}

Timer::Timer(Variable &stat)
    : stat(stat)
{
}

Timer::~Timer()
{
    stat.add(getElapsed());
}


static std::queue<std::pair<std::vector<cl::Event>, boost::reference_wrapper<Variable> > > savedEvents;
static boost::mutex savedEventsMutex;

static void flushEventTimes(bool finalize)
{
    const cl_profiling_info fields[2] =
    {
        CL_PROFILING_COMMAND_START,
        CL_PROFILING_COMMAND_END
    };

    while (!savedEvents.empty())
    {
        const std::vector<cl::Event> &events = savedEvents.front().first;
        Variable &stat = boost::unwrap_ref(savedEvents.front().second);
        double total = 0.0;
        bool good = true;

        for (std::size_t j = 0; j < events.size() && good; j++)
        {
            const cl::Event &event = events[j];
            if (event.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>() != CL_COMPLETE)
            {
                if (finalize)
                {
                    Log::log[Log::warn] << "Warning: Event for " << stat.getName() << " did not complete successfully\n";
                    good = false;
                    break;
                }
                else
                {
                    // The front item is not ready to be reaped yet
                    return;
                }
            }

            cl_int status;
            cl_ulong values[2];
            for (unsigned int i = 0; i < 2 && good; i++)
            {
                status = clGetEventProfilingInfo(event(), fields[i], sizeof(values[i]), &values[i], NULL);
                switch (status)
                {
                case CL_PROFILING_INFO_NOT_AVAILABLE:
                    good = false;
                    break;
                case CL_SUCCESS:
                    break;
                default:
                    Log::log[Log::warn] << "Warning: Could not extract profiling information for " << stat.getName() << '\n';
                    good = false;
                    break;
                }
            }

            if (good)
            {
                double duration = 1e-9 * (values[1] - values[0]);
                total += duration;
            }
        }

        if (good)
            stat.add(total);
        savedEvents.pop();
        getStatistic<Peak<std::size_t> >("events.peak") -= 1;
    }
}

void timeEvents(const std::vector<cl::Event> &events, Variable &stat)
{
    if (!events.empty())
    {
        boost::lock_guard<boost::mutex> lock(savedEventsMutex);
        savedEvents.push(std::make_pair(events, boost::ref(stat)));
        getStatistic<Peak<std::size_t> >("events.peak") += 1;
        flushEventTimes(false);
    }
}

void timeEvent(cl::Event event, Variable &stat)
{
    std::vector<cl::Event> events(1, event);
    timeEvents(events, stat);
}

void finalizeEventTimes()
{
    boost::lock_guard<boost::mutex> lock(savedEventsMutex);
    flushEventTimes(true);
}

Registry::Registry() : mutex()
{
}

Registry::~Registry()
{
}

Registry &Registry::getInstance()
{
    static Registry singleton;
    return singleton;
}

Registry::iterator Registry::begin()
{
    return iterator(statistics.begin());
}

Registry::iterator Registry::end()
{
    return iterator(statistics.end());
}

Registry::const_iterator Registry::begin() const
{
    return const_iterator(statistics.begin());
}

Registry::const_iterator Registry::end() const
{
    return const_iterator(statistics.end());
}

std::ostream &operator<<(std::ostream &o, const Registry &reg)
{
    boost::lock_guard<boost::mutex> _(reg.mutex);
    for (boost::ptr_map<std::string, Statistic>::const_iterator i = reg.statistics.begin(); i != reg.statistics.end(); ++i)
    {
        o << *i->second << '\n';
    }
    return o;
}

} // namespace Statistics

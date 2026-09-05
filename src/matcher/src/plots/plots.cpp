#include "plots.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace
{
// AI GENERATED
// Roughly the rdtsc granularity on zen2, ~29 cycles at 2.9GHz, so bars line up
// with the values the timer can actually produce.
constexpr uint32_t kBucketNs = 10;

// ----------------------------------------------------------------- canvas

// Maps data coordinates onto pixel coordinates inside an SVG frame.
struct Canvas
{
	double width        = 1920.0;
	double height       = 1080.0;
	double marginLeft   = 100.0;
	double marginRight  = 35.0;
	double marginTop    = 60.0;
	double marginBottom = 85.0;
	double fontScale    = 1.0;   // raise it with the canvas size to keep text legible

	double xMin = 0.0, xMax = 1.0, yMin = 0.0, yMax = 1.0;

	double left()   const { return marginLeft; }
	double right()  const { return width - marginRight; }
	double top()    const { return marginTop; }
	double bottom() const { return height - marginBottom; }

	double px(double x) const { return left() + (x - xMin) / (xMax - xMin) * (right() - left()); }
	double py(double y) const { return bottom() - (y - yMin) / (yMax - yMin) * (bottom() - top()); }
};

// ------------------------------------------------------------------ ticks

std::string PlainLabel(double v)
{
	if (v == 0.0)           return "0";   // std::format would print "-0" for negative zero
	if (std::abs(v) >= 1e6) return std::format("{:.1f}M", v / 1e6);
	if (std::abs(v) >= 1e4) return std::format("{:.0f}k", v / 1e3);
	return std::format("{:.0f}", v);
}

std::vector<double> NiceSteps(double lo, double hi, int wanted)
{
	const double rawStep    = (hi - lo) / wanted;
	const double magnitude  = std::pow(10.0, std::floor(std::log10(rawStep)));
	const double normalised = rawStep / magnitude;

	double step = magnitude;
	if (normalised > 5.0)      step = 10.0 * magnitude;
	else if (normalised > 2.0) step = 5.0 * magnitude;
	else if (normalised > 1.0) step = 2.0 * magnitude;

	std::vector<double> ticks;
	for (double t = std::ceil(lo / step) * step; t <= hi + step * 1e-9; t += step)
		ticks.push_back(t);
	return ticks;
}

// A gridline at an axis position. An empty label draws the line only, which is
// how minor log ticks stay readable.
struct Tick
{
	double      position;
	std::string label;
	bool        major = true;
};

// Labelled lines on the 1-2-5 steps, with minorsPerMajor-1 unlabelled lines
// subdividing each interval.
std::vector<Tick> LinearTicks(double lo, double hi, int wantedMajors, int minorsPerMajor)
{
	const std::vector<double> majors = NiceSteps(lo, hi, wantedMajors);

	std::vector<Tick> ticks;
	if (majors.size() < 2 || minorsPerMajor < 2)
	{
		for (const double t : majors)
			ticks.push_back({t, PlainLabel(t), true});
		return ticks;
	}

	const double step      = majors[1] - majors[0];
	const double minorStep = step / minorsPerMajor;

	// Start one step below the first major so the minors to its left are drawn.
	for (int k = -1;; ++k)
	{
		const double base = majors.front() + k * step;
		if (base > hi)
			break;

		if (base >= lo)
			ticks.push_back({base, PlainLabel(base), true});

		for (int i = 1; i < minorsPerMajor; ++i)
		{
			const double minor = base + i * minorStep;
			if (minor >= lo && minor <= hi)
				ticks.push_back({minor, "", false});
		}
	}
	return ticks;
}

// A 1-2-5 step almost never lands on the first data value, so the low end of an
// axis goes unlabelled. Label it, and drop any generated label near enough to
// collide with it.
void LabelAxisStart(std::vector<Tick>& ticks, double lo, double hi)
{
	const double tooClose = (hi - lo) * 0.03;

	std::erase_if(ticks, [lo, tooClose](const Tick& t)
	{
		return !t.label.empty() && std::abs(t.position - lo) < tooClose;
	});

	ticks.insert(ticks.begin(), {lo, PlainLabel(lo), true});
}

// Ticks for an axis holding log2(count): one labelled line per doubling. Counts
// are integers, so there is nothing to place between 1 and 2, which is why this
// axis carries no minor lines. Labels are exact rather than abbreviated, since a
// rounded 33k reads worse than 32768 on an axis made of powers of two.
std::vector<Tick> Log2Ticks(int lowestOctave, int highestOctave)
{
	std::vector<Tick> ticks;
	for (int octave = lowestOctave; octave <= highestOctave; ++octave)
	{
		const uint64_t count = uint64_t{1} << octave;
		ticks.push_back({static_cast<double>(octave), std::format("{}", count), true});
	}
	return ticks;
}

// ------------------------------------------------------------------- file

void WriteSvg(const std::filesystem::path& out,
              const Canvas& c,
              std::string_view title,
              std::string_view xLabel,
              std::string_view yLabel,
              const std::vector<Tick>& xTicks,
              const std::vector<Tick>& yTicks,
              std::string_view body)
{
	std::ofstream f(out);
	if (!f)
	{
		std::println("could not write {}", out.string());
		return;
	}

	std::string svg;

	svg += std::format(
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\" "
		"viewBox=\"0 0 {} {}\" font-family=\"monospace\">\n",
		c.width, c.height, c.width, c.height);
	svg += std::format("<rect width=\"{}\" height=\"{}\" fill=\"#ffffff\"/>\n", c.width, c.height);
	svg += std::format("<text x=\"{}\" y=\"{:.0f}\" font-size=\"{:.0f}\" fill=\"#111\">{}</text>\n",
	                   c.left(), 32 * c.fontScale, 19 * c.fontScale, title);

	// horizontal grid lines and Y tick labels
	for (const Tick& t : yTicks)
	{
		if (t.position < c.yMin || t.position > c.yMax)
			continue;

		const double y = c.py(t.position);
		svg += std::format("<line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" "
		                   "stroke=\"{}\"/>\n",
		                   c.left(), y, c.right(), y, t.major ? "#f0f0f0" : "#efefef");
		if (!t.label.empty())
			svg += std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"{:.0f}\" fill=\"{}\" "
			                   "text-anchor=\"end\">{}</text>\n",
			                   c.left() - 10.0, y + 4.0 * c.fontScale,
			                   (t.major ? 13 : 11) * c.fontScale,
			                   t.major ? "#444" : "#8a8a8a", t.label);
	}

	// vertical grid lines and X tick labels
	for (const Tick& t : xTicks)
	{
		if (t.position < c.xMin || t.position > c.xMax)
			continue;

		const double x = c.px(t.position);
		svg += std::format("<line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" "
		                   "stroke=\"{}\"/>\n",
		                   x, c.top(), x, c.bottom(), t.major ? "#d0d0d0" : "#efefef");
		if (!t.label.empty())
			svg += std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"{:.0f}\" fill=\"#444\" "
			                   "text-anchor=\"middle\">{}</text>\n",
			                   x, c.bottom() + 22.0 * c.fontScale, 13 * c.fontScale, t.label);
	}

	svg += body;

	// axis lines
	svg += std::format("<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"#111\"/>\n",
	                   c.left(), c.bottom(), c.right(), c.bottom());
	svg += std::format("<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"#111\"/>\n",
	                   c.left(), c.top(), c.left(), c.bottom());

	svg += std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"{:.0f}\" fill=\"#111\" "
	                   "text-anchor=\"middle\">{}</text>\n",
	                   (c.left() + c.right()) / 2.0, c.height - 25.0 * c.fontScale,
	                   14 * c.fontScale, xLabel);
	svg += std::format("<text x=\"22\" y=\"{:.1f}\" font-size=\"{:.0f}\" fill=\"#111\" "
	                   "text-anchor=\"middle\" transform=\"rotate(-90 22 {:.1f})\">{}</text>\n",
	                   (c.top() + c.bottom()) / 2.0, 14 * c.fontScale,
	                   (c.top() + c.bottom()) / 2.0, yLabel);

	svg += "</svg>\n";
	f << svg;
	std::println("plot written at {}", out.string());
}

// -------------------------------------------------------------- histogram

// Both histograms bucket the samples the same way and lay the bars out the same
// way. All they disagree on is how a bucket count becomes a bar height.
struct Buckets
{
	std::map<uint32_t, uint64_t> counts;   // bucket start ns -> frequency
	uint64_t maxCount = 0;

	double firstNs() const { return static_cast<double>(counts.begin()->first); }
	double lastNs()  const { return static_cast<double>(counts.rbegin()->first + kBucketNs); }
};

Buckets BucketByLatency(std::span<const matcher::Duration> durations)
{
	Buckets b;
	for (const matcher::Duration& d : durations)
	{
		const uint32_t ns = static_cast<uint32_t>(matcher::DurationNs(d));
		b.maxCount = std::max(b.maxCount, ++b.counts[ns / kBucketNs * kBucketNs]);
	}
	return b;
}

// One rect per occupied bucket, sitting on baselineY. barHeight turns a count
// into a pixel height, which is the only thing the two histograms differ on.
template <typename HeightFn>
std::string HistogramBars(const Buckets& b, const Canvas& c, double baselineY, double gapPx,
                          HeightFn barHeight)
{
	std::string body;
	for (const auto& [bucket, count] : b.counts)
	{
		const double x      = c.px(bucket);
		const double w      = c.px(bucket + kBucketNs) - x;
		const double height = barHeight(count);

		body += std::format("<rect x=\"{:.2f}\" y=\"{:.2f}\" width=\"{:.2f}\" height=\"{:.2f}\" "
		                    "fill=\"#3b6ea5\"/>\n",
		                    x, baselineY - height, std::max(w - gapPx, 0.5), height);
	}
	return body;
}

}   // namespace

namespace plots
{

void WriteLatencyHistogramLog(std::span<const matcher::Duration> durations,
                              const std::filesystem::path& out)
{
	if (durations.empty())
		return;

	const Buckets b = BucketByLatency(durations);

	// Bars are measured from a baseline of half a sample rather than from one,
	// because log2(1) is zero and a bucket holding a single sample would
	// otherwise have no height at all. One octave below 1 gives it a full
	// doubling of height.
	constexpr double baseline = -1.0;   // log2(0.5)

	const int topOctave = static_cast<int>(std::ceil(std::log2(static_cast<double>(b.maxCount))));

	const Canvas c{
		.xMin = b.firstNs(),
		.xMax = b.lastNs(),
		.yMin = baseline,
		.yMax = static_cast<double>(topOctave),
	};

	const std::string body = HistogramBars(b, c, c.py(baseline), 0.6,
		[&c, baseline](uint64_t count)
		{
			return c.py(baseline) - c.py(std::log2(static_cast<double>(count)));
		});

	std::vector<Tick> xTicks = LinearTicks(c.xMin, c.xMax, 24, 2);
	LabelAxisStart(xTicks, c.xMin, c.xMax);

	WriteSvg(out, c,
	         std::format(" {}, latency histogram, ({} samples, {}ns buckets)",
	                     out.generic_string(), durations.size(), kBucketNs),
	         "latency (ns)", "frequency (log2 scale)",
	         xTicks, Log2Ticks(0, topOctave), body);
}

void WriteLatencyHistogramLinear(std::span<const matcher::Duration> durations,
                                 const std::filesystem::path& out)
{
	if (durations.empty())
		return;

	constexpr double minBarPx = 6.0;   // floor for any bucket holding at least one sample

	const Buckets b = BucketByLatency(durations);

	const Canvas c{
		.xMin         = b.firstNs(),
		.xMax         = b.lastNs(),
		.yMin         = 0.0,
		.yMax         = static_cast<double>(b.maxCount),
	};

	std::string body = HistogramBars(b, c, c.py(0.0), 1.0,
		[&c, minBarPx](uint64_t count)
		{
			return std::max(c.py(0.0) - c.py(static_cast<double>(count)), minBarPx);
		});

	// Every count below this maps to fewer than minBarPx pixels and so is drawn
	// at the floor, indistinguishable from a count of one. Say where that line
	// is, so a floored bar is not read as a real frequency.
	const double flattenedBelow = minBarPx / (c.bottom() - c.top()) * static_cast<double>(b.maxCount);

	body += std::format("<line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" "
	                    "stroke=\"#d1495b\" stroke-width=\"1\" stroke-dasharray=\"5 4\"/>\n",
	                    c.left(), c.py(0.0) - minBarPx, c.right(), c.py(0.0) - minBarPx);
	body += std::format("<text x=\"{:.1f}\" y=\"{:.1f}\" font-size=\"20\" fill=\"#d1495b\" "
	                    "text-anchor=\"end\">floor, holds 1 to {:.0f} samples</text>\n",
	                    c.right(), c.py(0.0) - minBarPx - 14.0, flattenedBelow);

	std::vector<Tick> xTicks = LinearTicks(c.xMin, c.xMax, 48, 2);
	LabelAxisStart(xTicks, c.xMin, c.xMax);

	WriteSvg(out, c,
	         std::format("{}, latency histogram, linear scale  ({} samples, {}ns buckets)",
	                     out.generic_string(), durations.size(), kBucketNs),
	         "latency (ns)", "frequency",
	         xTicks, LinearTicks(c.yMin, c.yMax, 24, 5), body);
}

void WriteLatencyByOrderId(std::span<const matcher::Duration> durations,
                           const std::filesystem::path& out)
{
	if (durations.empty())
		return;

	constexpr size_t binCount = 240;

	uint64_t idMin = durations.front().id;
	uint64_t idMax = durations.front().id;
	for (const matcher::Duration& d : durations)
	{
		idMin = std::min<uint64_t>(idMin, d.id);
		idMax = std::max<uint64_t>(idMax, d.id);
	}

	struct Bin { uint64_t sum = 0; uint64_t n = 0; uint32_t worst = 0; };
	std::vector<Bin> bins(binCount);

	const uint64_t span = idMax - idMin + 1;
	for (const matcher::Duration& d : durations)
	{
		const uint32_t ns  = static_cast<uint32_t>(matcher::DurationNs(d));
		const size_t   idx = static_cast<size_t>((d.id - idMin) * binCount / span);

		Bin& b = bins[idx];
		b.sum += ns;
		++b.n;
		b.worst = std::max(b.worst, ns);
	}

	uint32_t worstOverall = 0;
	for (const Bin& b : bins)
		worstOverall = std::max(worstOverall, b.worst);

	Canvas c{
		.xMin = static_cast<double>(idMin),
		.xMax = static_cast<double>(idMax + 1),
		.yMin = 0.0,
		.yMax = worstOverall * 1.05,
	};

	const double binWidth = static_cast<double>(span) / binCount;

	std::string body;
	for (size_t i = 0; i < binCount; ++i)
	{
		const Bin& b = bins[i];
		if (b.n == 0)
			continue;

		const double x = c.px(static_cast<double>(idMin) + i * binWidth);
		const double w = std::max(c.px(static_cast<double>(idMin) + (i + 1) * binWidth) - x - 0.6, 0.5);

		const double worstY = c.py(b.worst);
		body += std::format("<rect x=\"{:.2f}\" y=\"{:.2f}\" width=\"{:.2f}\" height=\"{:.2f}\" "
		                    "fill=\"#c3d6e8\"/>\n", x, worstY, w, c.py(0.0) - worstY);

		const double meanY = c.py(static_cast<double>(b.sum) / b.n);
		body += std::format("<rect x=\"{:.2f}\" y=\"{:.2f}\" width=\"{:.2f}\" height=\"{:.2f}\" "
		                    "fill=\"#3b6ea5\"/>\n", x, meanY, w, c.py(0.0) - meanY);
	}

	body += std::format("<rect x=\"{}\" y=\"{}\" width=\"14\" height=\"14\" fill=\"#c3d6e8\"/>"
	                    "<text x=\"{}\" y=\"{}\" font-size=\"13\" fill=\"#444\">worst</text>\n",
	                    c.right() - 150.0, c.top() + 8.0, c.right() - 130.0, c.top() + 20.0);
	body += std::format("<rect x=\"{}\" y=\"{}\" width=\"14\" height=\"14\" fill=\"#3b6ea5\"/>"
	                    "<text x=\"{}\" y=\"{}\" font-size=\"13\" fill=\"#444\">mean</text>\n",
	                    c.right() - 70.0, c.top() + 8.0, c.right() - 50.0, c.top() + 20.0);

	WriteSvg(out, c,
	         std::format("{}, latency against order id  ({} bins)", out.generic_string(), binCount),
	         "order id", "latency (ns)",
	         LinearTicks(c.xMin, c.xMax, 20, 2), LinearTicks(c.yMin, c.yMax, 16, 2), body);
}

void WriteLatencyByFills(std::span<const matcher::Duration> durations,
                         const std::filesystem::path& out)
{
	if (durations.empty())
		return;

	struct Stats
	{
		uint64_t sum = 0;
		uint64_t n = 0;
		uint32_t lo = UINT32_MAX;
		uint32_t hi = 0;
	};

	std::map<uint32_t, Stats> byFills;
	uint32_t worstOverall = 0;
	for (const matcher::Duration& d : durations)
	{
		const uint32_t ns = static_cast<uint32_t>(matcher::DurationNs(d));

		Stats& st = byFills[d.fills];
		st.sum += ns;
		++st.n;
		st.lo = std::min(st.lo, ns);
		st.hi = std::max(st.hi, ns);
		worstOverall = std::max(worstOverall, ns);
	}

	Canvas c{
		.xMin = byFills.begin()->first - 1.0,
		.xMax = byFills.rbegin()->first + 1.0,
		.yMin = 0.0,
		.yMax = worstOverall * 1.05,
	};

	const double capHalfWidth = (c.px(1.0) - c.px(0.0)) * 0.3;

	std::string body;
	for (const auto& [fills, st] : byFills)
	{
		const double x    = c.px(fills);
		const double mean = static_cast<double>(st.sum) / st.n;

		// vertical range line, min to max
		body += std::format("<line x1=\"{:.2f}\" y1=\"{:.2f}\" x2=\"{:.2f}\" y2=\"{:.2f}\" "
		                    "stroke=\"#7b8ba0\" stroke-width=\"1.5\"/>\n",
		                    x, c.py(st.lo), x, c.py(st.hi));

		// caps at each end
		for (const uint32_t end : {st.lo, st.hi})
			body += std::format("<line x1=\"{:.2f}\" y1=\"{:.2f}\" x2=\"{:.2f}\" y2=\"{:.2f}\" "
			                    "stroke=\"#7b8ba0\" stroke-width=\"1.5\"/>\n",
			                    x - capHalfWidth, c.py(end), x + capHalfWidth, c.py(end));

		// mean marker
		body += std::format("<line x1=\"{:.2f}\" y1=\"{:.2f}\" x2=\"{:.2f}\" y2=\"{:.2f}\" "
		                    "stroke=\"#d1495b\" stroke-width=\"3\"/>\n",
		                    x - capHalfWidth, c.py(mean), x + capHalfWidth, c.py(mean));
	}

	body += std::format("<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"#d1495b\" "
	                    "stroke-width=\"3\"/>"
	                    "<text x=\"{}\" y=\"{}\" font-size=\"13\" fill=\"#444\">mean</text>\n",
	                    c.right() - 210.0, c.top() + 14.0, c.right() - 186.0, c.top() + 14.0,
	                    c.right() - 180.0, c.top() + 18.0);
	body += std::format("<line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"#7b8ba0\" "
	                    "stroke-width=\"1.5\"/>"
	                    "<text x=\"{}\" y=\"{}\" font-size=\"13\" fill=\"#444\">min..max</text>\n",
	                    c.right() - 120.0, c.top() + 6.0, c.right() - 120.0, c.top() + 22.0,
	                    c.right() - 112.0, c.top() + 18.0);

	WriteSvg(out, c, std::format("{}, latency spread per fill count", out.generic_string()),
	         "fills per order", "latency (ns)",
	         LinearTicks(c.xMin, c.xMax, 24, 2), LinearTicks(c.yMin, c.yMax, 16, 2), body);
}

}   // namespace plots

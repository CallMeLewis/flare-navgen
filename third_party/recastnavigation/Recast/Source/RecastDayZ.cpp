// DAYZ: Recast passes DayZ's navmeshes are built with.

#include <limits.h>
#include <string.h>

#include "Recast.h"
#include "RecastAlloc.h"
#include "RecastAssert.h"

// A 21x21 majority vote over the lowest span's area
// in each column, ignoring area 0; ties go to the lowest area. Run on the heightfield after terrain rasterisation.
// The window slides along each row, so a step only removes one column of 21 cells and adds another.
bool dzModeFilterWalkableAreaFlat(rcContext* ctx, rcHeightfield& hf, const int borderSize)
{
	const int w = hf.width;
	const int h = hf.height;
	const int n = w * h;
	unsigned char* src = (unsigned char*)rcAlloc(n, RC_ALLOC_TEMP);
	unsigned char* dst = (unsigned char*)rcAlloc(n, RC_ALLOC_TEMP);
	if (!src || !dst)
	{
		ctx->log(RC_LOG_ERROR, "dzModeFilterWalkableAreaFlat: Out of memory (%d cells).", n);
		rcFree(src);
		rcFree(dst);
		return false;
	}
	for (int i = 0; i < n; ++i)
		src[i] = hf.spans[i] ? (unsigned char)hf.spans[i]->area : 0;
	memcpy(dst, src, n);

	const int r = 10;
	const int half = (2 * r + 1) * (2 * r + 1) / 2;
	unsigned short hist[64]; // span areas are 6 bits
	for (int z = borderSize; z < h - borderSize; ++z)
	{
		memset(hist, 0, sizeof(hist));
		for (int dz = -r; dz <= r; ++dz)
		{
			const unsigned char* row = &src[(z + dz) * w + borderSize];
			for (int dx = -r; dx <= r; ++dx)
				hist[row[dx]]++;
		}
		int mode = 0;
		for (int x = borderSize; x < w - borderSize; ++x)
		{
			if (x > borderSize)
			{
				for (int dz = -r; dz <= r; ++dz)
				{
					const unsigned char* row = &src[(z + dz) * w];
					hist[row[x - r - 1]]--;
					hist[row[x + r]]++;
				}
			}
			// An area holding more than half the window can't be matched, so it stays the mode.
			if (mode == 0 || hist[mode] <= half)
			{
				unsigned short best = 0;
				mode = 0;
				for (int a = 1; a < 64; ++a)
				{
					if (hist[a] > best)
					{
						best = hist[a];
						mode = a;
					}
				}
			}
			dst[z * w + x] = (unsigned char)mode;
		}
	}
	for (int i = 0; i < n; ++i)
		if (hf.spans[i])
			hf.spans[i]->area = dst[i];
	rcFree(src);
	rcFree(dst);
	return true;
}

// Replaces rcFilterWalkableLowHeightSpans. Under a ceiling whose area is flagged
// (DayZ: 18, "WallWithHole"), the clearance picks one of several areas (unwalkable / crawl /
// crouch); under any other ceiling a clearance <= heights[count-1] makes the span unwalkable.
void dzFilterLowHeightSpansMulti(rcContext* ctx, const int* heights, const unsigned char* areas, const int count,
                                 const unsigned char* flaggedCeilings, rcHeightfield& hf)
{
	rcIgnoreUnused(ctx);
	if (count <= 0)
		return;
	const int w = hf.width;
	const int h = hf.height;
	for (int z = 0; z < h; ++z)
	{
		for (int x = 0; x < w; ++x)
		{
			rcSpan* s = hf.spans[x + z * w];
			if (!s)
				continue;
			for (rcSpan* next = s->next; next; s = next, next = next->next)
			{
				const int gap = (int)next->smin - (int)s->smax;
				if (!flaggedCeilings[next->area])
				{
					if (gap <= heights[count - 1])
						s->area = RC_NULL_AREA;
				}
				else
				{
					for (int i = 0; i < count; ++i)
					{
						if (gap <= heights[i])
						{
							s->area = areas[i];
							break;
						}
					}
				}
			}
		}
	}
}

// Two-pass chamfer distance (orthogonal 2, diagonal 3) as in rcErodeWalkableArea. Each
// improvement calls onImprove(target, source).
template <typename OnImprove>
static void dzChamfer(const rcCompactHeightfield& chf, unsigned char* dist, OnImprove&& onImprove)
{
	const int w = chf.width;
	const int h = chf.height;
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const rcCompactCell& c = chf.cells[x + y * w];
			for (int i = (int)c.index, ni = (int)(c.index + c.count); i < ni; ++i)
			{
				const rcCompactSpan& s = chf.spans[i];
				if (rcGetCon(s, 0) != RC_NOT_CONNECTED)
				{
					const int ax = x + rcGetDirOffsetX(0);
					const int ay = y + rcGetDirOffsetY(0);
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon(s, 0);
					const rcCompactSpan& as = chf.spans[ai];
					if ((int)dist[ai] + 2 < (int)dist[i])
					{
						dist[i] = (unsigned char)(dist[ai] + 2);
						onImprove(i, ai);
					}
					if (rcGetCon(as, 3) != RC_NOT_CONNECTED)
					{
						const int aax = ax + rcGetDirOffsetX(3);
						const int aay = ay + rcGetDirOffsetY(3);
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon(as, 3);
						if ((int)dist[aai] + 3 < (int)dist[i])
						{
							dist[i] = (unsigned char)(dist[aai] + 3);
							onImprove(i, aai);
						}
					}
				}
				if (rcGetCon(s, 3) != RC_NOT_CONNECTED)
				{
					const int ax = x + rcGetDirOffsetX(3);
					const int ay = y + rcGetDirOffsetY(3);
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon(s, 3);
					const rcCompactSpan& as = chf.spans[ai];
					if ((int)dist[ai] + 2 < (int)dist[i])
					{
						dist[i] = (unsigned char)(dist[ai] + 2);
						onImprove(i, ai);
					}
					if (rcGetCon(as, 2) != RC_NOT_CONNECTED)
					{
						const int aax = ax + rcGetDirOffsetX(2);
						const int aay = ay + rcGetDirOffsetY(2);
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon(as, 2);
						if ((int)dist[aai] + 3 < (int)dist[i])
						{
							dist[i] = (unsigned char)(dist[aai] + 3);
							onImprove(i, aai);
						}
					}
				}
			}
		}
	}
	for (int y = h - 1; y >= 0; --y)
	{
		for (int x = w - 1; x >= 0; --x)
		{
			const rcCompactCell& c = chf.cells[x + y * w];
			for (int i = (int)c.index, ni = (int)(c.index + c.count); i < ni; ++i)
			{
				const rcCompactSpan& s = chf.spans[i];
				if (rcGetCon(s, 2) != RC_NOT_CONNECTED)
				{
					const int ax = x + rcGetDirOffsetX(2);
					const int ay = y + rcGetDirOffsetY(2);
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon(s, 2);
					const rcCompactSpan& as = chf.spans[ai];
					if ((int)dist[ai] + 2 < (int)dist[i])
					{
						dist[i] = (unsigned char)(dist[ai] + 2);
						onImprove(i, ai);
					}
					if (rcGetCon(as, 1) != RC_NOT_CONNECTED)
					{
						const int aax = ax + rcGetDirOffsetX(1);
						const int aay = ay + rcGetDirOffsetY(1);
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon(as, 1);
						if ((int)dist[aai] + 3 < (int)dist[i])
						{
							dist[i] = (unsigned char)(dist[aai] + 3);
							onImprove(i, aai);
						}
					}
				}
				if (rcGetCon(s, 1) != RC_NOT_CONNECTED)
				{
					const int ax = x + rcGetDirOffsetX(1);
					const int ay = y + rcGetDirOffsetY(1);
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon(s, 1);
					const rcCompactSpan& as = chf.spans[ai];
					if ((int)dist[ai] + 2 < (int)dist[i])
					{
						dist[i] = (unsigned char)(dist[ai] + 2);
						onImprove(i, ai);
					}
					if (rcGetCon(as, 0) != RC_NOT_CONNECTED)
					{
						const int aax = ax + rcGetDirOffsetX(0);
						const int aay = ay + rcGetDirOffsetY(0);
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon(as, 0);
						if ((int)dist[aai] + 3 < (int)dist[i])
						{
							dist[i] = (unsigned char)(dist[aai] + 3);
							onImprove(i, aai);
						}
					}
				}
			}
		}
	}
}

// Grows `area` by `radius` cells into spans whose current area is set
// in `mask` (including unwalkable spans when mask[0]), carrying the part id along.
bool dzDilateArea(rcContext* ctx, const int radius, rcCompactHeightfield& chf, const unsigned char area,
                  const unsigned char* mask)
{
	unsigned char* dist = (unsigned char*)rcAlloc(chf.spanCount, RC_ALLOC_TEMP);
	if (!dist)
	{
		ctx->log(RC_LOG_ERROR, "dilateArea: Out of memory 'dist' (%d).", chf.spanCount);
		return false;
	}
	unsigned short* sub = (unsigned short*)rcAlloc(sizeof(unsigned short) * chf.spanCount, RC_ALLOC_TEMP);
	if (!sub)
	{
		ctx->log(RC_LOG_ERROR, "dilateArea: Out of memory 'subArea' (%d).", chf.spanCount);
		rcFree(dist);
		return false;
	}
	memset(dist, 0xff, chf.spanCount);
	for (int i = 0; i < chf.spanCount; ++i)
	{
		if (chf.areas[i] == area)
			dist[i] = 0;
		sub[i] = chf.subAreas[i];
	}
	dzChamfer(chf, dist, [&](int i, int from) { sub[i] = sub[from]; });
	const unsigned char thr = (unsigned char)(radius * 2);
	for (int i = 0; i < chf.spanCount; ++i)
	{
		if (mask[chf.areas[i]] && dist[i] <= thr)
		{
			chf.areas[i] = area;
			chf.subAreas[i] = sub[i];
		}
	}
	rcFree(dist);
	rcFree(sub);
	return true;
}

// Turns tree parts (area `treeArea`, part id = subArea) into
// cylinder obstacles. Trees with more than `minClearance` metres to the next blocker and a
// footprint no bigger than `maxSize` metres become walkable (`walkArea`) plus an obstacle
// {x, y, z, radius}; the others become unwalkable. Returns the number of obstacles written.
int dzExtractTreeObstacles(rcContext* ctx, rcCompactHeightfield& chf, const unsigned char treeArea,
                           const unsigned char walkArea, const int partCount, const unsigned char* seedAreas,
                           const float minClearance, const float maxSize, float* outObstacles)
{
	struct SpanData
	{
		unsigned short area;
		unsigned short sub;
		unsigned char dist;
	};
	SpanData* sd = (SpanData*)rcAlloc(sizeof(SpanData) * chf.spanCount, RC_ALLOC_TEMP);
	if (!sd)
	{
		ctx->log(RC_LOG_ERROR, "spGetAreaDistances: Out of memory 'spanDatas' (%d).", chf.spanCount);
		return 0;
	}
	unsigned short* minDist = (unsigned short*)rcAlloc(sizeof(unsigned short) * rcMax(partCount, 1), RC_ALLOC_TEMP);
	if (!minDist)
	{
		ctx->log(RC_LOG_ERROR, "spGetAreaDistances: Out of memory 'minDist' (%d).", partCount);
		rcFree(sd);
		return 0;
	}
	memset(minDist, 0xff, sizeof(unsigned short) * rcMax(partCount, 1));

	const int w = chf.width;
	const int h = chf.height;
	for (int i = 0; i < chf.spanCount; ++i)
	{
		sd[i].dist = seedAreas[chf.areas[i]] ? 0 : 0xff;
		sd[i].area = chf.areas[i];
		sd[i].sub = chf.subAreas[i];
	}
	unsigned char* dist = (unsigned char*)rcAlloc(chf.spanCount, RC_ALLOC_TEMP);
	if (!dist)
	{
		rcFree(sd);
		rcFree(minDist);
		return 0;
	}
	for (int i = 0; i < chf.spanCount; ++i)
		dist[i] = sd[i].dist;
	dzChamfer(chf, dist, [&](int i, int from) {
		sd[i].area = sd[from].area;
		sd[i].sub = sd[from].sub;
	});
	for (int i = 0; i < chf.spanCount; ++i)
		sd[i].dist = dist[i];
	rcFree(dist);

	// Clearance of each tree: where its Voronoi cell meets another seed's, the sum of both
	// distances; at the edge of the walkable area, its own distance.
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const rcCompactCell& c = chf.cells[x + y * w];
			for (int i = (int)c.index, ni = (int)(c.index + c.count); i < ni; ++i)
			{
				if (sd[i].area != treeArea)
					continue;
				const rcCompactSpan& s = chf.spans[i];
				for (int dir = 0; dir < 4; ++dir)
				{
					unsigned short d;
					if (rcGetCon(s, dir) == RC_NOT_CONNECTED)
					{
						d = sd[i].dist;
					}
					else
					{
						const int ax = x + rcGetDirOffsetX(dir);
						const int ay = y + rcGetDirOffsetY(dir);
						const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon(s, dir);
						if (sd[ai].area == sd[i].area && sd[ai].sub == sd[i].sub)
							continue;
						d = (unsigned short)(sd[ai].dist + sd[i].dist);
					}
					if (sd[i].sub < partCount && d < minDist[sd[i].sub])
						minDist[sd[i].sub] = d;
				}
			}
		}
	}

	struct Obj
	{
		int count, sx, sy, sz, minx, minz, maxx, maxz;
	};
	Obj* objs = (Obj*)rcAlloc(sizeof(Obj) * rcMax(partCount, 1), RC_ALLOC_TEMP);
	if (!objs)
	{
		ctx->log(RC_LOG_ERROR, "spGetAreaDistances: Out of memory 'objData' (%d).", partCount * 8);
		rcFree(sd);
		rcFree(minDist);
		return 0;
	}
	for (int p = 0; p < partCount; ++p)
	{
		Obj& o = objs[p];
		o.count = o.sx = o.sy = o.sz = 0;
		o.minx = w;
		o.minz = h;
		o.maxx = o.maxz = 0;
	}
	const float cs = chf.cs;
	const unsigned short thr = (unsigned short)(int)((minClearance + minClearance) / cs);
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const rcCompactCell& c = chf.cells[x + y * w];
			for (int i = (int)c.index, ni = (int)(c.index + c.count); i < ni; ++i)
			{
				const unsigned short p = chf.subAreas[i];
				if (chf.areas[i] != treeArea || p >= partCount || !(thr < minDist[p]))
					continue;
				Obj& o = objs[p];
				o.count++;
				o.sx += x;
				o.sy += chf.spans[i].y;
				o.sz += y;
				o.minx = rcMin(o.minx, x);
				o.minz = rcMin(o.minz, y);
				o.maxx = rcMax(o.maxx, x);
				o.maxz = rcMax(o.maxz, y);
			}
		}
	}
	const int maxCells = (int)(maxSize / cs);
	for (int p = 0; p < partCount; ++p)
	{
		Obj& o = objs[p];
		if (!o.count)
			continue;
		int size = o.maxz - o.minz;
		const int wx = o.maxx - o.minx;
		if (size + 1 <= wx)
			size = wx;
		if (maxCells < size + 1)
		{
			minDist[p] = 0;
			o.count = 0;
		}
		else
		{
			o.minx = size + 1;  // reused as the footprint in cells
		}
	}
	for (int i = 0; i < chf.spanCount; ++i)
	{
		if (chf.areas[i] != treeArea)
			continue;
		const unsigned short p = sd[i].sub;
		chf.areas[i] = (p < partCount && thr < minDist[p]) ? walkArea : RC_NULL_AREA;
		chf.subAreas[i] = 0;
	}
	int n = 0;
	for (int p = 0; p < partCount; ++p)
	{
		const Obj& o = objs[p];
		if (!o.count)
			continue;
		float* out = &outObstacles[n * 4];
		out[0] = ((float)o.sx / (float)o.count + (float)chf.borderSize + 0.5f) * chf.cs + chf.bmin[0];
		out[1] = ((float)o.sy / (float)o.count + (float)chf.borderSize + 0.5f) * chf.ch + chf.bmin[1];
		out[2] = ((float)o.sz / (float)o.count + (float)chf.borderSize + 0.5f) * chf.cs + chf.bmin[2];
		out[3] = (float)o.minx * chf.cs * 0.5f;
		++n;
	}
	rcFree(objs);
	rcFree(minDist);
	rcFree(sd);
	return n;
}

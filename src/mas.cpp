// mas.cpp - simple attempt to write a multiple sequence alignment application

#include "mas.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <limits>
#include <cmath>
#include <numeric>

#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH
#include <boost/tr1/tuple.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>

#include "ioseq.h"
#include "matrix.h"
#include "buffer.h"
#include "utils.h"
#include "guide.h"
#include "align-3d.h"

using namespace std;
using namespace tr1;
namespace fs = boost::filesystem;
namespace po = boost::program_options;
namespace io = boost::iostreams;
namespace ba = boost::algorithm;

int //VERBOSE = 0,
	MULTI_THREADED = 1;

// --------------------------------------------------------------------

aa kAA_Reverse[256];

// ah, too bad those lamda's are not supported yet...
struct sum_weight
{
	float operator()(float sum, const entry* e) const { return sum + e->m_weight; }
};

struct max_pdb_nr
{
	int16 operator()(int16 a, int16 b) const { return max(a, b); }
};

// --------------------------------------------------------------------

void entry::insert_gap(uint32 pos)
{
	if (pos > m_seq.length())
	{
		m_seq += kSignalGapCode;
		if (not m_positions.empty())
			m_positions.push_back(0);
	}
	else
	{
		aa r = kSignalGapCode;
		m_seq.insert(pos, &r, 1);
		if (not m_positions.empty())
			m_positions.insert(m_positions.begin() + pos, 0);
	}

	assert(m_positions.size() == m_seq.length() or m_positions.empty());
}

void entry::append_gap()
{
	m_seq += kSignalGapCode;
	if (not m_positions.empty())
		m_positions.push_back(0);

	assert(m_positions.size() == m_seq.length() or m_positions.empty());
}

void entry::remove_gaps()
{
	if (m_seq.length() == m_positions.size())
		assert(false);
	else
		m_seq.erase(remove(m_seq.begin(), m_seq.end(), kSignalGapCode), m_seq.end());
}

// --------------------------------------------------------------------

ostream& operator<<(ostream& lhs, base_node& rhs)
{
	rhs.print(lhs);
	return lhs;
}

joined_node::joined_node(base_node* left, base_node* right,
	float d_left, float d_right)
	: m_left(left)
	, m_right(right)
	, m_d_left(d_left)
	, m_d_right(d_right)
	, m_leaf_count(left->leaf_count() + right->leaf_count())
	, m_length(max(left->length(), right->length()))
{
	m_left->add_weight(d_left / m_left->leaf_count());
	m_right->add_weight(d_right / m_right->leaf_count());
}

joined_node::~joined_node()
{
	delete m_left;
	delete m_right;
}

void joined_node::print(ostream& s)
{
	s << '(' << endl
	  << *m_left << (boost::format(":%1.4f") % m_d_left) << ',' << endl
	  << *m_right << (boost::format(":%1.4f") % m_d_right) << ')' << endl;
}

void leaf_node::print(ostream& s)
{
	s << m_entry.m_id;
}

// --------------------------------------------------------------------
// distance is calculated as 1 minus the fraction of identical residues

const substitution_matrix kDistanceMatrix("GONNET250");
const float kDistanceGapOpen = 10;
const float kDistanceGapExtend = 0.2f;

float calculateDistance(const entry& a, const entry& b)
{
	int32 x = 0, endX = 0, dimX = a.m_seq.length();
	int32 y = 0, endY = 0, dimY = b.m_seq.length();

	const vector<int16>& pa = a.m_positions;
	const vector<int16>& pb = b.m_positions;
	
	matrix<float>	B(dimX, dimY);
	matrix<float>	Ix(dimX, dimY);
	matrix<float>	Iy(dimX, dimY);
	matrix<uint16>	id(dimX, dimY);
	
	Ix(0, 0) = 0;
	Iy(0, 0) = 0;
	
	uint16 highId = 0;
	
	if (pa.empty() or pb.empty())
	{
		endX = dimX;
		endY = dimY;
	}

	while (x < dimX and y < dimY)
	{
		if (x == endX and y == endY)
		{
			if (pa[x] == pb[y] and pa[x] != 0)
			{
				if (a.m_seq[x] == b.m_seq[y])
					++highId;
				++x;	++endX;
				++y;	++endY;
				continue;
			}
		}

		while (endX < dimX or endY < dimY)
		{
			if (endX < dimX and pa[endX] == 0)
			{
				++endX;
				continue;
			}

			if (endY < dimY and pb[endY] == 0)
			{
				++endY;
				continue;
			}

			if (endX < dimX and endY < dimY and pa[endX] == pb[endY] and pa[endX] != 0)
				break;
			
			if (endX < dimX)
			{
				while (endX < dimX and (endY == dimY or pa[endX] < pb[endY]))
					++endX;
			}
			
			if (endY < dimY)
			{
				while (endY < dimY and (endX == dimX or pb[endY] < pa[endX]))
					++endY;
			}
			
			if (endX < dimX and endY < dimY and pa[endX] != pb[endY])
				continue;

			break;
		}

		Ix(x, y) = 0;
		Iy(x, y) = 0;
		if (x > 0 and y > 0)
			id(x - 1, y - 1) = highId;

		int32 startX = x, startY = y;
		float high = -numeric_limits<float>::max();
		uint16 highIdSub = 0;

		for (x = startX; x < endX; ++x)
		{
			for (y = startY; y < endY; ++y)
			{
				float Ix1 = 0; if (x > startX) Ix1 = Ix(x - 1, y);
				float Iy1 = 0; if (y > startY) Iy1 = Iy(x, y - 1);
	
				// (1)
				float M = kDistanceMatrix(a.m_seq[x], b.m_seq[y]);
				if (x > startX and y > startY)
					M += B(x - 1, y - 1);
	
				float s;
				uint16 i = 0;
				if (a.m_seq[x] == b.m_seq[y])
					i = 1;
	
				if (M >= Ix1 and M >= Iy1)
				{
					if (x > startX and y > startY)
						i += id(x - 1, y - 1);
					s = M;
				}
				else if (Ix1 >= Iy1)
				{
					if (x > startX)
						i += id(x - 1, y);
					s = Ix1;
				}
				else
				{
					if (y > startY)
						i += id(x, y - 1);
					s = Iy1;
				}
				
				B(x, y) = s;
				id(x, y) = i;
				
				if ((x == endX - 1 or y == endY - 1) and high < s)
				{
					high = s;
					highIdSub = i;
				}
	
				// (3)
				Ix(x, y) = max(M - kDistanceGapOpen, Ix1 - kDistanceGapExtend);
				
				// (4)
				Iy(x, y) = max(M - kDistanceGapOpen, Iy1 - kDistanceGapExtend);
			}
		}

		highId += highIdSub;

		x = endX;
		y = endY;
	}
	
	float result = 1.0f - float(highId) / max(dimX, dimY);

	assert(result >= 0.0f);
	assert(result <= 1.0f);
	
	if (VERBOSE)
	{
		static boost::mutex sLockCout;
		boost::mutex::scoped_lock lock(sLockCout);
		cerr << (boost::format("Sequences (%1$d:%2$d) Aligned. Score: %3$4.2f") % (a.m_nr + 1) % (b.m_nr + 1) % result) << endl;
		if (VERBOSE >= 2)
			cerr << "  " << a.m_id << ':' << b.m_id << endl;
	}
	
	return result;
}

// we use as many threads as is useful to do the distance calculation
// which is quite easy to do using a thread safe queue
typedef buffer<tr1::tuple<uint32,uint32> > 	distance_queue;
const tr1::tuple<uint32,uint32>	kSentinel = tr1::make_tuple(numeric_limits<uint32>::max(), 0);

void calculateDistance(distance_queue& queue, symmetric_matrix<float>& d, vector<entry>& data,
	progress& pr)
{
	for (;;)
	{
		uint32 a, b;
		tr1::tie(a, b) = queue.get();

		if (a == numeric_limits<uint32>::max()) // sentinel found, quit loop
			break;
	
		d(a, b) = calculateDistance(data[a], data[b]);
		pr.step();
	}
	
	queue.put(kSentinel);
}

void calculateDistanceMatrix(symmetric_matrix<float>& d, vector<entry>& data)
{
	progress pr("calculating guide tree", (data.size() * (data.size() - 1)) / 2);
	distance_queue queue;

	boost::thread_group t;

	uint32 nr_of_threads = boost::thread::hardware_concurrency();

	if (not MULTI_THREADED)
		nr_of_threads = 1;

	for (uint32 ti = 0; ti < nr_of_threads; ++ti)
		t.create_thread(boost::bind(&calculateDistance,
			boost::ref(queue), boost::ref(d), boost::ref(data), boost::ref(pr)));
	
	for (uint32 a = 0; a < data.size() - 1; ++a)
	{
		for (uint32 b = a + 1; b < data.size(); ++b)
			queue.put(tr1::make_tuple(a, b));
	}
	
	queue.put(kSentinel);
	
	t.join_all();
}

// --------------------------------------------------------------------

string decode(const sequence& s)
{
	string result;
	result.reserve(s.length());
	
	foreach (aa a, s)
		result.push_back(kAA[a]);

	return result;
}

sequence encode(const string& s)
{
	static bool sInited = false;
	static uint8 kAA_Reverse[256];
	
	if (not sInited)
	{
		// init global reverse mapping
		for (uint32 a = 0; a < 256; ++a)
			kAA_Reverse[a] = 255;
		for (uint8 a = 0; a < sizeof(kAA); ++a)
		{
			kAA_Reverse[toupper(kAA[a])] = a;
			kAA_Reverse[tolower(kAA[a])] = a;
		}
	}
	
	sequence result;
	result.reserve(s.length());

	foreach (char r, s)
	{
		if (r == '.' or r == '*' or r == '~')
			r = '-';
		
		aa rc = kAA_Reverse[static_cast<uint8>(r)];
		if (rc >= sizeof(kAA))
			throw mas_exception(boost::format("invalid residue in sequence %1%") % r);

		result.push_back(rc);
	}
	
	return result;
}

// --------------------------------------------------------------------

void joinNeighbours(symmetric_matrix<float>& d, vector<base_node*>& tree)
{
	uint32 r = tree.size();
	
	while (r > 2)
	{
		// calculate the sums first
		vector<float> sum(r);
		for (uint32 i = 1; i < r; ++i)
		{
			for (uint32 j = 0; j < i; ++j)
			{
				float dij = d(i, j);
				sum[i] += dij;
				sum[j] += dij;
			}
		}
		
		// calculate Q, or in fact, the position of the minimum in Q
		uint32 min_i = 0, min_j = 0;
		float m = numeric_limits<float>::max();
		
		for (uint32 i = 1; i < r; ++i)
		{
			for (uint32 j = 0; j < i; ++j)
			{
				float v = d(i, j) - (sum[i] + sum[j]) / (r - 2);
	
				if (m > v)
				{
					min_i = i;
					min_j = j;
					m = v;
				}
			}
		}
		

		// distance to joined node
		float d_i, d_j;
		float half_dij = d(min_i, min_j) / 2;
		
		d_i = half_dij + abs(sum[min_i] - sum[min_j]) / (2 * (r - 2));
		d_j = d(min_i, min_j) - d_i;
	
		if (d_i > d_j and tree[min_i]->leaf_count() > tree[min_j]->leaf_count())
			swap(d_i, d_j);
	
		joined_node* jn = new joined_node(tree[min_i], tree[min_j], d_i, d_j);
		assert(min_j < min_i);
		tree.erase(tree.begin() + min_i);
		tree.erase(tree.begin() + min_j);
		tree.push_back(jn);
		
		// distances to other nodes
		vector<float> dn; dn.reserve(r - 2);
		for (uint32 x = 0; x < r; ++x)
		{
			if (x == min_i or x == min_j)
				continue;
			dn.push_back((abs(d(x, min_i) - d_i) + abs(d(x, min_j) - d_j)) / 2);
		}
		
		// fill new distance matrix
		d.erase_2(min_i, min_j);
		--r;
		for (uint32 x = 0; x < r - 1; ++x)
			d(x, r - 1) = dn[x];
	}

	assert(r == 2); assert(tree.size() == 2);

	joined_node* root = new joined_node(tree[0], tree[1], d(0, 1) / 2, d(0, 1) / 2);
	tree.clear();
	tree.push_back(root);
}

// --------------------------------------------------------------------

inline float score(const vector<entry*>& a, const vector<entry*>& b,
	uint32 ix_a, uint32 ix_b, const substitution_matrix& mat)
{
	float result = 0;
	
	foreach (const entry* ea, a)
	{
		foreach (const entry* eb, b)
		{
			assert(ix_a < ea->m_seq.length());
			assert(ix_b < eb->m_seq.length());
			
			aa ra = ea->m_seq[ix_a];
			aa rb = eb->m_seq[ix_b];
			
			if (ra != kSignalGapCode and rb != kSignalGapCode)
				result += ea->m_weight * eb->m_weight * mat(ra, rb);
		}
	}

	result /= (a.size() * b.size());
	
	return result;
}

// don't ask me, but looking at the clustal code, they substract 0.2 from the table
// as mentioned in the article in NAR.
const float kResidueSpecificPenalty[20] = {
	1.13f - 0.2f,		// A
	0.72f - 0.2f,		// R
	0.63f - 0.2f,		// N
	0.96f - 0.2f,		// D
	1.13f - 0.2f,		// C
	1.07f - 0.2f,		// Q
	1.31f - 0.2f,		// E
	0.61f - 0.2f,		// G
	1.00f - 0.2f,		// H
	1.32f - 0.2f,		// I
	1.21f - 0.2f,		// L
	0.96f - 0.2f,		// K
	1.29f - 0.2f,		// M
	1.20f - 0.2f,		// F
	0.74f - 0.2f,		// P
	0.76f - 0.2f,		// S
	0.89f - 0.2f,		// T
	1.23f - 0.2f,		// W
	1.00f - 0.2f,		// Y
	1.25f - 0.2f		// V
};

const boost::function<bool(aa)> is_hydrophilic = ba::is_any_of(encode("DEGKNQPRS"));

void adjust_gp(vector<float>& gop, vector<float>& gep, const vector<entry*>& seq)
{
	assert(gop.size() == seq.front()->m_seq.length());

	vector<uint32> gaps(gop.size());
	vector<bool> hydrophilic_stretch(gop.size(), false);
	vector<float> residue_specific_penalty(gop.size());

	foreach (const entry* e, seq)
	{
		const sequence& s = e->m_seq;
		const string& ss = e->m_ss;
		
		for (uint32 ix = 0; ix < gop.size(); ++ix)
		{
			aa r = s[ix];
			
			if (r == kSignalGapCode)
				gaps[ix] += 1;

			// residue specific gap penalty
			if (ix < ss.length())
			{
				// The output of DSSP is explained extensively under 'explanation'. The very short summary of the output is: 
				// H = alpha helix 
				// B = residue in isolated beta-bridge 
				// E = extended strand, participates in beta ladder 
				// G = 3-helix (3/10 helix) 
				// I = 5 helix (pi helix) 
				// T = hydrogen bonded turn 
				// S = bend

				switch (ss[ix])
				{
					case 'H':
					case 'G':
					case 'I':
						residue_specific_penalty[ix] += 3.0f;
						break;

					case 'B':
						residue_specific_penalty[ix] += 2.0f;
						break;

					case 'E':
						residue_specific_penalty[ix] += 1.5f;
						break;

					default:
						residue_specific_penalty[ix] += 1.0f;
						break;
				}
			}
			else if (r < 20)
				residue_specific_penalty[ix] += kResidueSpecificPenalty[r];
			else
				residue_specific_penalty[ix] += 1.0f;
		}
		
		// find a run of 5 hydrophilic residues
		for (uint32 si = 0, i = 0; i <= gop.size(); ++i)
		{
			if (i == gop.size() or is_hydrophilic(s[i]) == false)
			{
				if (i >= si + 5)
				{
					for (uint32 j = si; j < i; ++j)
						hydrophilic_stretch[j] = true;
				}
				si = i + 1;
			}
		}
	}

	for (int32 ix = 0; ix < static_cast<int32>(gop.size()); ++ix)
	{
		// if there is a gap, lower gap open cost
		if (gaps[ix] > 0)
		{
			gop[ix] *= 0.3f * ((seq.size() - gaps[ix]) / float(seq.size()));
			gep[ix] /= 2;
		}
		
		// else if there is a gap within 8 residues, increase gap cost
		else
		{
			for (int32 d = 0; d < 8; ++d)
			{
				if (ix + d >= int32(gaps.size()) or gaps[ix + d] > 0 or
					ix - d < 0 or gaps[ix - d] > 0)
				{
					gop[ix] *= (2 + ((8 - d) * 2)) / 8.f;
					break;
				}
			}
			
			if (hydrophilic_stretch[ix])
				gop[ix] /= 3;
			else
				gop[ix] *= (residue_specific_penalty[ix] / seq.size());
		}
	}
}

void print_matrix(ostream& os, const matrix<int8>& tb, const sequence& sx, const sequence& sy)
{
	os << ' ';
	for (uint32 x = 0; x < sx.length(); ++x)
		os << kAA[sx[x]];
	os << endl;

	for (uint32 y = 0; y < sy.length(); ++y)
	{
		os << kAA[sy[y]];
		for (uint32 x = 0; x < sx.length(); ++x)
		{
			switch (tb(x, y))
			{
				case -1:	os << '|'; break;
				case 0:		os << '\\'; break;
				case 1:		os << '-'; break;
				case 2:		os << '.'; break;
			}
		}
		os << endl;
	}
}

void align(
	const joined_node* node,
	vector<entry*>& a, vector<entry*>& b, vector<entry*>& c,
	const substitution_matrix_family& mat_fam,
	float gop, float gep, float magic,
	bool ignorePositions)
{
	if (VERBOSE > 2)
	{
		cerr << "aligning sets" << endl << "a(" << a.front()->m_seq.length() << "): ";
		foreach (const entry* e, a)
			cerr << e->m_id << "; ";
		cerr << endl << "b(" << b.front()->m_seq.length() << "): ";
		foreach (const entry* e, b)
			cerr << e->m_id << "; ";
		cerr << endl << endl;
	}
	
	const float kSentinelValue = -(numeric_limits<float>::max() / 2);
	
	const entry* fa = a.front();
	const entry* fb = b.front();
	
	const vector<int16>& pa = fa->m_positions;
	const vector<int16>& pb = fb->m_positions;

	int32 x = 0, dimX = fa->m_seq.length(), endX = 0;
	int32 y = 0, dimY = fb->m_seq.length(), endY = 0;
	
#ifdef NDEBUG
	matrix<float> B(dimX, dimY);
	matrix<float> Ix(dimX, dimY);
	matrix<float> Iy(dimX, dimY);
	matrix<int8> tb(dimX, dimY);
#else
	matrix<float> B(dimX, dimY, kSentinelValue);
	matrix<float> Ix(dimX, dimY);
	matrix<float> Iy(dimX, dimY);
	matrix<int8> tb(dimX, dimY, 2);
#endif
	
	const substitution_matrix& smat = mat_fam(abs(node->m_d_left + node->m_d_right), true);

	float minLength = static_cast<float>(dimX), maxLength = static_cast<float>(dimY);
	if (minLength > maxLength)
		swap(minLength, maxLength);
	
	float logmin = 1.0f / log10(minLength);
	float logdiff = 1.0f + 0.5f * log10(minLength / maxLength);
	
	// initial gap open cost, 0.05f is the remaining magical number here...
	gop = (gop / (logdiff * logmin)) * abs(smat.mismatch_average()) * smat.scale_factor() * magic;

	float avg_weight_a = accumulate(a.begin(), a.end(), 0.f, sum_weight()) / a.size();
	float avg_weight_b = accumulate(b.begin(), b.end(), 0.f, sum_weight()) / b.size();

	// position specific gap penalties
	// initial gap extend cost is adjusted for difference in sequence lengths
	vector<float> gop_a(dimX, gop * avg_weight_a),
		gep_a(dimX, gep * (1 + log10(float(dimX) / dimY)) * avg_weight_a);
	adjust_gp(gop_a, gep_a, a);
	
	vector<float> gop_b(dimY, gop * avg_weight_b),
		gep_b(dimY, gep * (1 + log10(float(dimY) / dimX)) * avg_weight_b);
	adjust_gp(gop_b, gep_b, b);

	// normally, startX is 0 and endX is dimX, however, when there are fixed
	// positions, we only take into account the sub matrices that are allowed
	
	if (ignorePositions or pa.empty() or pb.empty())
	{
		endX = dimX;
		endY = dimY;
	}

	int32 highX = 0, highY = 0;

	while (x < dimX and y < dimY)
	{
		if (x == endX and y == endY)
		{
			if (pa[x] == pb[y] and pa[x] != 0)
			{
				tb(x, y) = 0;
				highX = x;
				highY = y;
				++x;	++endX;
				++y;	++endY;
				continue;
			}
		}

		while (endX < dimX or endY < dimY)
		{
			if (endX < dimX and pa[endX] == 0)
			{
				++endX;
				continue;
			}

			if (endY < dimY and pb[endY] == 0)
			{
				++endY;
				continue;
			}

			if (endX < dimX and endY < dimY and pa[endX] == pb[endY] and pa[endX] != 0)
				break;
			
			if (endX < dimX)
			{
				while (endX < dimX and (endY == dimY or pa[endX] < pb[endY]))
					++endX;
			}
			
			if (endY < dimY)
			{
				while (endY < dimY and (endX == dimX or pb[endY] < pa[endX]))
					++endY;
			}
			
			if (endX < dimX and endY < dimY and pa[endX] != pb[endY])
				continue;

			break;
		}

		Ix(x, y) = 0;
		Iy(x, y) = 0;
	
		float high = 0;
		int32 startX = x, startY = y;

		if (y > 0)
		{
			for (int32 ix = x; ix < endX; ++ix)
				tb(ix, y - 1) = 1;
		}

		if (x > 0)
		{
			for (int32 iy = y; iy < endY; ++iy)
				tb(x - 1, iy) = -1;
		}
		
		for (x = startX; x < endX; ++x)
		{
			for (y = startY; y < endY; ++y)
			{
				float Ix1 = 0; if (x > startX) Ix1 = Ix(x - 1, y);
				float Iy1 = 0; if (y > startY) Iy1 = Iy(x, y - 1);
				
				float M = score(a, b, x, y, smat);
				if (x > startX and y > startY)
					M += B(x - 1, y - 1);
	
				float s;
				if (M >= Ix1 and M >= Iy1)
				{
					tb(x, y) = 0;
					B(x, y) = s = M;
				}
				else if (Ix1 >= Iy1)
				{
					tb(x, y) = 1;
					B(x, y) = s = Ix1;
				}
				else
				{
					tb(x, y) = -1;
					B(x, y) = s = Iy1;
				}
				
				if ((x == endX - 1 or y == endY - 1) and high <= s)
				{
					high = s;
					highX = x;
					highY = y;
				}
				
				Ix(x, y) = max(M - (x < dimX - 1 ? gop_a[x] : 0), Ix1 - gep_a[x]);
				Iy(x, y) = max(M - (y < dimY - 1 ? gop_b[y] : 0), Iy1 - gep_b[y]);
			}
		}

		if (endY > 0)
		{
			for (x = highX + 1; x < endX; ++x)
				tb(x, endY - 1) = 1;
		}

		if (endX > 0)
		{
			for (y = highY + 1; y < endY; ++y)
				tb(endX - 1, y) = -1;
		}
		
		x = endX;
		y = endY;
	}
	
	if (endY > 0)
	{
		for (x = highX + 1; x < dimX; ++x)
			tb(x, endY - 1) = 1;
	}

	if (endX > 0)
	{
		for (y = highY + 1; y < dimY; ++y)
			tb(endX - 1, y) = -1;
	}

	// build the alignment
	x = dimX - 1;
	y = dimY - 1;

	if (VERBOSE >= 6)
		print_matrix(cerr, tb, fa->m_seq, fb->m_seq);

	// trace back the matrix
	while (x >= 0 and y >= 0)
	{
		switch (tb(x, y))
		{
			case -1:
				foreach (entry* e, a)
					e->insert_gap(x + 1);
				--y;
				break;

			case 1:
				foreach (entry* e, b)
					e->insert_gap(y + 1);
				--x;
				break;

			case 0:
				--x;
				--y;
				break;
			
			default:
				assert(false);
				break;
		}
	}
	
	// and finally insert start-gaps
	while (x >= 0)
	{
		foreach (entry* e, b)
			e->insert_gap(y + 1);
		--x;
	}

	while (y >= 0)
	{
		foreach (entry* e, a)
			e->insert_gap(x + 1);
		--y;
	}

	c.reserve(a.size() + b.size());
	copy(a.begin(), a.end(), back_inserter(c));
	copy(b.begin(), b.end(), back_inserter(c));
	
	// copy over the pdb_nrs to the first line
	if (not ignorePositions and not pa.empty())
	{
		assert(pa.size() == pb.size());
		vector<int16>& pc = c.front()->m_positions;
		
		transform(
			pa.begin(), pa.end(),
			pb.begin(),
			pc.begin(),
			max_pdb_nr());
	}
	
	if (VERBOSE >= 2)
		report(c, cerr, "clustalw");
}

void createAlignment(joined_node* node, vector<entry*>& alignment,
	const substitution_matrix_family& mat, float gop, float gep, float magic,
	progress& pr)
{
	vector<entry*> a, b;

	if (MULTI_THREADED)
	{
		boost::thread_group t;

		if (dynamic_cast<leaf_node*>(node->left()) != NULL)
			a.push_back(&static_cast<leaf_node*>(node->left())->m_entry);
		else
			t.create_thread(boost::bind(&createAlignment,
				static_cast<joined_node*>(node->left()), boost::ref(a), boost::ref(mat), gop, gep, magic, 
				boost::ref(pr)));

		if (dynamic_cast<leaf_node*>(node->right()) != NULL)
			b.push_back(&static_cast<leaf_node*>(node->right())->m_entry);
		else
			t.create_thread(boost::bind(&createAlignment,
				static_cast<joined_node*>(node->right()), boost::ref(b), boost::ref(mat), gop, gep, magic, 
				boost::ref(pr)));
	
		t.join_all();
	}
	else
	{
		if (dynamic_cast<leaf_node*>(node->left()) != NULL)
			a.push_back(&static_cast<leaf_node*>(node->left())->m_entry);
		else
			createAlignment(static_cast<joined_node*>(node->left()), a, mat, gop, gep, magic, pr);

		if (dynamic_cast<leaf_node*>(node->right()) != NULL)
			b.push_back(&static_cast<leaf_node*>(node->right())->m_entry);
		else
			createAlignment(static_cast<joined_node*>(node->right()), b, mat, gop, gep, magic, pr);
	}

	align(node, a, b, alignment, mat, gop, gep, magic, false);
	
	pr.step(node->cost());
}

int main(int argc, char* argv[])
{
	try
	{
		po::options_description desc("mas options");
		desc.add_options()
			("help,h",							 "Display help message")
			("input,i",		po::value<string>(), "Input file")
			("outfile,o",	po::value<string>(), "Output file, use 'stdout' to output to screen")
			("format,f",	po::value<string>(), "Output format, can be clustalw (default) or fasta")
			("outtree",		po::value<string>(), "Write guide tree")
			("debug,d",		po::value<int>(),	 "Debug output level")
			("verbose,v",						 "Verbose output")
			("no-threads,T",					 "Avoid multi-threading (=debug option)")
			("guide-tree,g",po::value<string>(), "use existing guide tree")
			("matrix,m",	po::value<string>(), "Substitution matrix, default is PAM")
			("gap-open",	po::value<float>(),	 "Gap open penalty")
			("gap-extend",	po::value<float>(),	 "Gap extend penalty")
			("magic",		po::value<float>(),	 "Magical number")
			("chain,c",		po::value<char>(),	 "Chain ID to select (from HSSP input)")
			("ignore-pos-nr",					 "Do not use position/PDB nr in scoring")
			("3d-a",		po::value<string>(), "Align-3d file A")
			("3d-b",		po::value<string>(), "Align-3d file B")
			("iterations,I",po::value<uint32>(), "Number of iterations in 3d alignment")
			("ss",								 "Read secondary structure files")
			("dssp",		po::value<string>(), "Create DSSP for <pdbid>")
			;
	
		po::positional_options_description p;
		p.add("input", -1);
	
		po::variables_map vm;
		po::store(po::command_line_parser(argc, argv).options(desc).positional(p).run(), vm);
		po::notify(vm);

		VERBOSE = vm.count("verbose");
		if (vm.count("debug"))
			VERBOSE = vm["debug"].as<int>();

		if (vm.count("no-threads"))
			MULTI_THREADED = 0;
		
		if (vm.count("dssp"))
		{
			test_ss(vm["dssp"].as<string>());
			exit(0);
		}
		
		if (vm.count("help") or (vm.count("input") == 0 and vm.count("3d-a") == 0))
		{
			cerr << desc << endl;
			exit(1);
		}
		
		// matrix
		string matrix;
		float gop, gep, magic;
		
		if (vm.count("input"))		// regular alignments
		{
			matrix = "BLOSUM";
			gop = 10;
			gep = 0.2;
			magic = 0.1;
		}
		else						// 3d alignments
		{
			matrix = "GONNET";
			gop = 2;
			gep = 0.02;
			magic = 0.1;
		}

		if (vm.count("matrix"))
			matrix = vm["matrix"].as<string>();
		substitution_matrix_family mat(matrix);

		if (vm.count("gap-open"))	gop = vm["gap-open"].as<float>();
		if (vm.count("gap-extend"))	gep = vm["gap-extend"].as<float>();
		if (vm.count("magic"))		magic = vm["magic"].as<float>();

		fs::path path;
		vector<entry> data;
		
		char chain = 0;
		if (vm.count("chain"))
			chain = vm["chain"].as<char>();

		if (vm.count("3d-a") and vm.count("3d-b"))
		{
//			if (vm.count("3d-a") != vm.count("3d-b"))
//			{
//				cerr << desc << endl;
//				exit(1);
//			}
			
			uint32 iterations = 5;
			if (vm.count("iterations"))
				iterations = vm["iterations"].as<uint32>();
			
			align_structures(vm["3d-a"].as<string>(), vm["3d-b"].as<string>(),
				iterations, mat, gop, gep, magic);
			return 0;
		}
		else
		{
			path = vm["input"].as<string>();

			if (path.extension() == ".hssp" or chain != 0)
				readAlignmentFromHsspFile(path, chain, data);
			else if (path.extension() == ".mapping")
				readWhatifMappingFile(path, data);
			else if (path.extension() == ".ids")
				readFamilyIdsFile(path, data);
			else
				readFasta(path, data);
			
			if (vm.count("ss"))
				readSecStruct(data);
		}
		
		if (vm.count("ignore-pos-nr"))
			for_each(data.begin(), data.end(), boost::bind(&entry::dump_positions, _1));
		
		if (data.size() < 2)
			throw mas_exception("insufficient number of sequences");

		vector<entry*> alignment;
		joined_node* root;
		
		if (data.size() == 2)
		{
			// no need to do difficult stuff, just align two sequences:
			float dist = calculateDistance(data[0], data[1]);
			root = new joined_node(new leaf_node(data[0]), new leaf_node(data[1]), dist / 2, dist / 2);
		}
		else
		{
			// create the leaf nodes
			vector<base_node*> tree;
			tree.reserve(data.size());
			foreach (entry& e, data)
				tree.push_back(new leaf_node(e));
			
			// calculate guide tree
			if (vm.count("guide-tree"))
				useGuideTree(vm["guide-tree"].as<string>(), tree);
			else
			{
				// a distance matrix
				symmetric_matrix<float> d(data.size());
				calculateDistanceMatrix(d, data);
				joinNeighbours(d, tree);
			}
			
			root = static_cast<joined_node*>(tree.front());
		}

		fs::path outfile;
		if (vm.count("outfile") == 0)
		{
			string name = path.stem();
			if (chain != 0)
			{
				name += '-';
				name += chain;
			}
			
			outfile = path.parent_path() / (name + ".aln");
		}
		else
			outfile = vm["outfile"].as<string>();
		
//		if (vm.count("outtree"))
//		{
//			fs::path treepath = vm["outtree"].as<string>();
//			treepath = treepath.parent_path() / (treepath.stem() + ".dnd");
//			fs::ofstream file(treepath);
//			if (not file.is_open())
//				throw mas_exception(boost::format("Failed to write guide tree %1%") % treepath.string());
//			file << *root << ';' << endl;
//		}

		if (VERBOSE)
			cerr << *root << ';' << endl;
		
		progress pr("calculating alignments", root->cumulative_cost());
		createAlignment(root, alignment, mat, gop, gep, magic, pr);
		
		sort(alignment.begin(), alignment.end(),
			boost::bind(&entry::nr, _1) < boost::bind(&entry::nr, _2));
		
		string format = "clustalw";
		if (vm.count("format"))
			format = vm["format"].as<string>();
		
		if (outfile == "stdout")
			report(alignment, cout, format);
		else
		{
			fs::ofstream file(outfile);
			if (not file.is_open())
				throw mas_exception(boost::format("failed to open output file %1%") % vm["outfile"].as<string>());
			report(alignment, file, format);
		}

		delete root;
	}
	catch (const exception& e)
	{
		cerr << e.what() << endl;
		exit(1);
	}
	
	return 0;
}


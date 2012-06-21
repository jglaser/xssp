//  Copyright Maarten L. Hekkelman, Radboud University 2011.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "mas.h"

#include <cmath>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/bzip2.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/date_time/date_clock_device.hpp>
#include <boost/regex.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/pool/pool_alloc.hpp>

// our includes
#include "buffer.h"
#include "matrix.h"
#include "dssp.h"
#include "structure.h"
#include "utils.h"
#include "hmmer-hssp.h"
#include "mkhssp.h"		// for our globals

#if P_WIN
#pragma warning (disable: 4267)
#endif

using namespace std;
namespace ba = boost::algorithm;
namespace io = boost::iostreams;

// --------------------------------------------------------------------
// utility routine
	
inline bool is_gap(char aa)
{
	return kResidueIX[uint8(aa)] == -2;
	// == '-' or aa == '~' or aa == '.' or aa == '_' or aa == ' ';
}

// --------------------------------------------------------------------
// basic named sequence type and a multiple sequence alignment container

struct insertion
{
	uint32			m_ipos, m_jpos;
	string			m_seq;
};
	
class seq
{
  public:
				seq(const seq&);
				//seq(const string& id);
				seq(const string& id, const string& desc);
				~seq();
				
	seq&		operator=(const seq&);

	void		swap(seq& o);

	string		id() const							{ return m_impl->m_id; }
	string		acc() const							{ return m_impl->m_acc; }
	string		pdb() const							{ return m_impl->m_pdb; }
	string		desc() const						{ return m_impl->m_desc; }

	float		identity() const					{ return m_impl->m_identical; }
	float		similarity() const					{ return m_impl->m_similar; }

	uint32		ifir() const						{ return m_impl->m_ifir; }
	uint32		ilas() const						{ return m_impl->m_ilas; }
	uint32		jfir() const						{ return m_impl->m_jfir; }
	uint32		jlas() const						{ return m_impl->m_jlas; }
	uint32		gapn() const						{ return m_impl->m_gapn; }
	uint32		gaps() const						{ return m_impl->m_gaps; }
	
	uint32		alignment_begin() const				{ return m_impl->m_begin; }
	uint32		alignment_end() const				{ return m_impl->m_end; }

	uint32		alignment_length() const			{ return m_impl->m_length; }
	uint32		seqlen() const						{ return m_impl->m_seqlen; }
	
	const list<insertion>&
				insertions() const					{ return m_impl->m_insertions; }

	void		append(const string& seq);
	void		cut(uint32 pos, uint32 n);

	void		update(const seq& qseq);
	static void	update_all(buffer<seq*>& b, const seq& qseq);

	bool		operator<(const seq& o) const		{ return m_impl->m_score > o.m_impl->m_score or
														(m_impl->m_score == o.m_impl->m_score and length() > o.length()); }

	uint32		length() const						{ return m_impl->m_end - m_impl->m_begin; }

	char&		operator[](uint32 offset)
				{
					assert(offset < m_impl->m_size);
					return m_impl->m_seq[offset];
				}

	char		operator[](uint32 offset) const
				{
					assert(offset < m_impl->m_size);
					return m_impl->m_seq[offset];
				}

	template<class T>
	class basic_iterator : public std::iterator<bidirectional_iterator_tag,T>
	{
	  public:
		typedef typename std::iterator<std::bidirectional_iterator_tag, T>	base_type;
		typedef	typename base_type::reference								reference;
		typedef typename base_type::pointer									pointer;

						basic_iterator(T* s) : m_seq(s) {}
						basic_iterator(const basic_iterator& o) : m_seq(o.m_seq) {}

		basic_iterator&	operator=(const basic_iterator& o)
						{
							m_seq = o.m_seq;
							return *this;
						}

		reference		operator*()					{ return *m_seq; }
		reference		operator->()				{ return *m_seq; }

		basic_iterator&	operator++()				{ ++m_seq; return *this; }
		basic_iterator	operator++(int)				{ basic_iterator iter(*this); operator++(); return iter; }

		basic_iterator&	operator--()				{ --m_seq; return *this; }
		basic_iterator	operator--(int)				{ basic_iterator iter(*this); operator--(); return iter; }

		bool			operator==(const basic_iterator& o) const
													{ return m_seq == o.m_seq; }
		bool			operator!=(const basic_iterator& o) const
													{ return m_seq != o.m_seq; }
	
		template<class U>
		friend basic_iterator<U> operator-(basic_iterator<U>, int);

	  private:
		pointer			m_seq;
	};
	
	typedef basic_iterator<char>		iterator;
	typedef basic_iterator<const char>	const_iterator;
	
	iterator		begin()							{ return iterator(m_impl->m_seq); }
	iterator		end()							{ return iterator(m_impl->m_seq + m_impl->m_size); }

	const_iterator	begin() const					{ return const_iterator(m_impl->m_seq); }
	const_iterator	end() const						{ return const_iterator(m_impl->m_seq + m_impl->m_size); }

  private:

	struct seq_impl
	{
					seq_impl(const string& id, const string& desc);
					~seq_impl();

		void		update(const seq_impl& qseq);
		void		cut(uint32 pos, uint32 n);

		iterator	begin()							{ return iterator(m_seq); }
		iterator	end()							{ return iterator(m_seq + m_size); }
	
		const_iterator
					begin() const					{ return const_iterator(m_seq); }
		const_iterator
					end() const						{ return const_iterator(m_seq + m_size); }

		string		m_id, m_acc, m_desc;
		uint32		m_ifir, m_ilas, m_jfir, m_jlas, m_length;
		float		m_identical, m_similar;
		uint32		m_begin, m_end;
		uint32		m_gaps, m_gapn;
		list<insertion>
					m_insertions;
		char*		m_data;
		char*		m_seq;
		uint32		m_refcount;
		uint32		m_size, m_space;
	};

	seq_impl*	m_impl;
	
				seq();
};

template<class T>
inline seq::basic_iterator<T> operator-(seq::basic_iterator<T> i, int o)
{
	seq::basic_iterator<T> r(i);
	r.m_seq -= o;
	return r;
}

//typedef boost::ptr_vector<seq> mseq;
typedef vector<seq>				mseq;

const uint32 kBlockSize = 512;

seq::seq_impl::seq_impl(const string& id, const string& desc)
	: m_id(id)
	, m_id2(id)
	, m_desc(desc)
	, m_identical(0)
	, m_similar(0)
	, m_length(0)
	, m_score(0)
	, m_begin(0)
	, m_end(0)
	, m_pruned(false)
	, m_gaps(0)
	, m_gapn(0)
	, m_seq(nullptr)
	, m_refcount(1)
	, m_size(0)
	, m_space(0)
{
	m_ifir = m_ilas = m_jfir = m_jlas = 0;
	m_data = m_seq = nullptr;
}

seq::seq_impl::~seq_impl()
{
	assert(m_refcount == 0);
	delete m_data;
}

seq::seq(const seq& s)
	: m_impl(s.m_impl)
{
	++m_impl->m_refcount;
}

seq::seq(const string& id, const string& desc)
	: m_impl(new seq_impl(id, desc))
{
	static const boost::regex re1("([-a-zA-Z0-9_]+)/(\\d+)-(\\d+)"),
				  			  re2("(?:tr|sp)\\|([[:alnum:]]+)\\|(.+)");
	boost::smatch sm;

	if (boost::regex_match(m_impl->m_id2, sm, re2))
	{
		m_impl->m_acc = sm[1];
		m_impl->m_id2 = sm[2];
	}

	if (boost::regex_match(m_impl->m_id2, sm, re1))
	{
		// jfir/jlas can be taken over from jackhmmer output
		m_impl->m_jfir = boost::lexical_cast<uint32>(sm.str(2));
		m_impl->m_jlas = boost::lexical_cast<uint32>(sm.str(3));

		m_impl->m_id2 = sm[1];
	}
}

seq& seq::operator=(const seq& rhs)
{
	if (this != &rhs)
	{
		if (--m_impl->m_refcount == 0)
			delete m_impl;
		
		m_impl = rhs.m_impl;
		
		++m_impl->m_refcount;
	}

	return *this;
}

seq::~seq()
{
	if (--m_impl->m_refcount == 0)
		delete m_impl;
}

void seq::swap(seq& o)
{
	std::swap(m_impl, o.m_impl);
}

void seq::append(const string& seq)
{
	if (m_impl->m_size + seq.length() > m_impl->m_space)
	{
		// increase storage for the sequences
		uint32 k = m_impl->m_space;
		if (k == 0)
			k = kBlockSize;
		uint32 n = k * 2;
		if (n < seq.length())
			n = seq.length();
		char* p = new char[n];
		memcpy(p, m_impl->m_data, m_impl->m_size);
		delete [] m_impl->m_data;
		m_impl->m_data = m_impl->m_seq = p;
		m_impl->m_space = n;
	}

	memcpy(m_impl->m_seq + m_impl->m_size, seq.c_str(), seq.length());
	m_impl->m_end = m_impl->m_size += seq.length();

	//const char* s = seq.c_str();
	//uint32 l = seq.length();
	//
	//while (l > 0)
	//{
	//	uint32 o = m_size % sizeof(fragment);
	//	
	//	if (o == 0)
	//		m_seq.push_back(fragment());
	//	
	//	uint32 k = l;
	//	if (k > sizeof(fragment) - o)
	//		k = sizeof(fragment) - o;
	//	
	//	char* d = m_seq.back().m_char;
	//	memcpy(d + o, s, k);
	//	
	//	m_size += k;
	//	l -= k;
	//}
}

void seq::cut(uint32 pos, uint32 n)
{
	m_impl->cut(pos, n);
}

void seq::seq_impl::cut(uint32 pos, uint32 n)
{
	assert(pos + n <= m_size);

	m_seq += pos;
	m_size = n;

	if (m_begin > pos)
		m_begin -= pos;
	else
		m_begin = 0;
	
	if (m_end > pos)
		m_end -= pos;
	else
		m_end = 0;

	if (m_end > m_size)
		m_end = m_size;
}

void seq::update_all(buffer<seq*>& b, const seq& qseq)
{
	for (;;)
	{
		seq* s = b.get();
		if (s == nullptr)
			break;

		s->update(qseq);
	}

	b.put(nullptr);
}

void seq::update(const seq& qseq)
{
	m_impl->update(*qseq.m_impl);
}

void seq::seq_impl::update(const seq_impl& qseq)
{
	uint32 ipos = 1, jpos = m_jfir;
	if (jpos == 0)
		jpos = 1;

	bool sgapf = false, qgapf = false;
	uint32 gapn = 0, gaps = 0;
	
	const_iterator qi = qseq.begin();
	iterator si = begin();
	uint32 i = 0;
	insertion ins = {};
	
	// reset statistics
	m_ifir = m_similar = m_identical = m_gapn = m_gaps = 0;
	m_begin = numeric_limits<uint32>::max();
	m_end = 0;
	
	uint32 length = 0;
	
	for (; qi != qseq.end(); ++qi, ++si, ++i)
	{
		bool qgap = is_gap(*qi);
		bool sgap = is_gap(*si);

		if (qgap and sgap)
			continue;

		// only update alignment length when we have started
		if (length > 0)
			++length;

		if (sgap)
		{
			if (not (sgapf or qgapf))
				++gaps;
			sgapf = true;
			++gapn;
			++ipos;

			continue;
		}
		else if (qgap)
		{
			if (not qgapf)
			{
				iterator gsi = si - 1;
				while (gsi != begin() and is_gap(*gsi))
					--gsi;
				
				ins.m_ipos = ipos;
				ins.m_jpos = jpos;
				ins.m_seq = *gsi = tolower(*gsi);
			}

			ins.m_seq += *si;
			
			if (not (sgapf or qgapf))
				++gaps;

			qgapf = true;
			++gapn;
			++jpos;
		}
		else
		{
			if (qgapf)
			{
				*si = tolower(*si);
				ins.m_seq += *si;
				m_insertions.push_back(ins);
			}
			
			sgapf = false;
			qgapf = false;

			m_ilas = ipos;
			if (m_ifir == 0)	// alignment didn't start yet
			{
				m_ifir = ipos;
				length = 1;
			}
			else
			{
				// no gaps in s or q, update gap counters and length
				m_gapn += gapn;
				m_gaps += gaps;
				m_length = length;
			}

			gaps = 0; // reset gap info
			gapn = 0;

			++ipos;
			++jpos;
		}

		if (*qi == *si)
			++m_identical;
		
		// validate the sequences while counting similarity
		uint8 rq = kResidueIX[static_cast<uint8>(*qi)];
		if (rq == -1)
			THROW(("Invalid letter in query sequence (%c)", *qi));
		uint8 rs = kResidueIX[static_cast<uint8>(*si)];
		if (rs == -1)
			THROW(("Invalid letter in query sequence (%c)", *si));
		
		if (rq >= 0 and rs >= 0 and kD(rq, rs) >= 0)
			++m_similar;

		if (m_begin == numeric_limits<uint32>::max())
			m_begin = i;
		
		m_end = i + 1;
	}
	
	if (m_begin == numeric_limits<uint32>::max())
		m_begin = m_end = 0;
	else
	{
		assert(m_begin <= m_size);
		assert(m_end <= m_size);

		for (i = 0; i < m_size; ++i)
		{
			if (i < m_begin or i >= m_end)
				m_seq[i] = ' ';
			else if (is_gap(m_seq[i]))
				m_seq[i] = '.';
		}
	}

	m_score = float(m_identical) / m_length;
}

bool seq::drop(float inThreshold) const
{
	uint32 ix = max(10U, min(m_impl->m_length, 80U)) - 10;
	
	bool result = m_impl->m_score < kHomologyThreshold[ix] + inThreshold;
	
	if (result and VERBOSE > 2)
		cerr << "dropping " << m_impl->m_id << " because identity " << m_impl->m_score << " is below threshold " << kHomologyThreshold[ix] << endl;
	
	return result;
}

namespace std
{
	template<>
	void swap(hmmer::seq& a, hmmer::seq& b)
	{
		a.swap(b);
	}
}

// --------------------------------------------------------------------
// ResidueHInfo is a class to store information about a residue in the
// original query sequence, along with statistics.

struct ResidueHInfo
{
					ResidueHInfo(uint32 seqNr);
					ResidueHInfo(char a, uint32 pos, char chain, uint32 seqNr, uint32 pdbNr,
						const string& dssp);

	void			CalculateVariability(hit_list& hits);

	char			letter;
	char			chain;
	string			dssp;
	uint32			seqNr, pdbNr;
	uint32			pos;
	uint32			nocc, ndel, nins;
	float			entropy, consweight;
	uint32			dist[20];
};

typedef shared_ptr<ResidueHInfo>						res_ptr;
typedef vector<res_ptr>									res_list;
typedef boost::iterator_range<res_list::iterator>::type	res_range;

// --------------------------------------------------------------------

void ReadStockholm(istream& is, string& id, string& header, mseq& msa, res_list& residues, const string& q)
{
	string line, qseq, qr;
	getline(is, line);
	if (line != "# STOCKHOLM 1.0")
		throw mas_exception("Not a stockholm file, missing first line");

	uint32 ix = 0, n = 0;
	
	for (;;)
	{
		line.clear();
		getline(is, line);
		
		if (line.empty())
		{
			if (not is.good())
				THROW(("Stockholm file is truncated or incomplete"));
			continue;
		}
		
		if (line == "//")
			break;

		if (ba::starts_with(line, "#=GF CC PDBID "))
		{
			id = line.substr(14);
			continue;
		}

		if (ba::starts_with(line, "#=GF CC DATE   ") or
			ba::starts_with(line, "#=GF CC PDBID  ") or
			ba::starts_with(line, "#=GF CC HEADER ") or
			ba::starts_with(line, "#=GF CC COMPND ") or
			ba::starts_with(line, "#=GF CC AUTHOR ") or
			ba::starts_with(line, "#=GF CC DBREF  "))
		{
			header += line.substr(15) + '\n';
			continue;
		}

		if (ba::starts_with(line, "#=RI "))
		{
			
			continue;
		}
	
		if (ba::starts_with(line, "#=GS "))
		{
			string id = line.substr(5), desc;
			string::size_type s = id.find("DE ");
			if (s != string::npos)
			{
				desc = id.substr(s + 3);
				id = id.substr(0, s);
			}
			
			ba::trim(id);
			if (msa.size() > 1 or msa.front().id() != id)
				msa.push_back(seq(id, desc));
			continue;
		}
		
		if (line[0] != '#')
		{
			string::size_type s = line.find(' ');
			if (s == string::npos)
				throw mas_exception("Invalid stockholm file");
			
			string id = line.substr(0, s);
			
			while (s < line.length() and line[s] == ' ')
				++s;
			
			string sseq = line.substr(s);
			
			if (id == msa[0].id())
			{
				ix = 0;
				qseq = sseq;
				n += sseq.length();
				
				foreach (char r, qseq)
				{
					if (not is_gap(r))
						qr += r;
				}
			}
			else
			{
				++ix;
				if (ix >= msa.size())
					msa.push_back(seq(id, ""));

				if (ix < msa.size() and id != msa[ix].id())
					THROW(("Invalid Stockholm file, ID does not match (%s != %s)", id.c_str(), msa[ix].id().c_str()));
			}

			if (ix < msa.size())
				msa[ix].append(sseq);
		}
	}
	
	if (msa.size() < 2)
		THROW(("Insufficient sequences in Stockholm MSA"));

	if (VERBOSE)
		cerr << " done, alignment width = " << n << ", nr of hits = " << msa.size() << endl << "Checking for threshold...";

	// first cut the msa, if needed:
	if (not q.empty() and q != qr)
	{
		if (qr.length() < q.length())
			THROW(("Query used for Stockholm file is too short for the chain"));

		string::size_type offset = qr.find(q);
		if (offset == string::npos)
			THROW(("Invalid Stockholm file for chain"));
		
		seq::iterator r = msa.front().begin();
		uint32 pos = 0;
		for (; r != msa.front().end(); ++r)
		{
			if (is_gap(*r) or offset-- > 0)
			{
				++pos;
				continue;
			}
			break;
		}
		
		uint32 n = 0, length = q.length();
		for (; r != msa.front().end(); ++r)
		{
			if (is_gap(*r) or length-- > 0)
			{
				++n;
				continue;
			}
			break;
		}

		foreach (seq& s, msa)
			s.cut(pos, n);
	}
	
	// update seq counters, try to do this multi threaded
	if (gNrOfThreads > 1)
	{
		buffer<seq*> b;
		boost::thread_group threads;
		for (uint32 t = 0; t < gNrOfThreads; ++t)
			threads.create_thread(boost::bind(&seq::update_all, boost::ref(b), boost::ref(msa.front())));
		
		for (uint32 i = 1; i < msa.size(); ++i)
			b.put(&msa[i]);
	
		b.put(nullptr);
		threads.join_all();
	}
	else
		for_each(msa.begin() + 1, msa.end(), boost::bind(&seq::update, _1, msa.front()));

	if (VERBOSE)
		cerr << "done" << endl;
}

// --------------------------------------------------------------------
// Hit is a class to store hit information and all of its statistics.
	
struct Hit
{
					Hit(CDatabankPtr inDatabank, seq& s, seq& q, char chain, uint32 offset);
					~Hit();

	seq&			m_seq;
	seq&			m_qseq;
	char			m_chain;
	uint32			m_nr, m_ifir, m_ilas, m_offset;
	float			m_ide, m_wsim;

	bool			operator<(const Hit& rhs) const
					{
						return m_ide > rhs.m_ide or
							(m_ide == rhs.m_ide and m_seq.alignment_length() > rhs.m_seq.alignment_length()) or
							(m_ide == rhs.m_ide and m_seq.alignment_length() == rhs.m_seq.alignment_length() and m_seq.id2() > rhs.m_seq.id2());
					}
};

typedef shared_ptr<Hit> hit_ptr;
typedef vector<hit_ptr>	hit_list;

// Create a Hit object based on a jackhmmer alignment pair
// first is the original query sequence, with gaps introduced.
// second is the hit sequence.
// Since this is jackhmmer output, we can safely assume the
// alignment does not contain gaps at the start or end of the query.
Hit::Hit(CDatabankPtr inDatabank, seq& s, seq& q, char chain, uint32 offset)
	: m_seq(s)
	, m_qseq(q)
	, m_chain(chain)
	, m_nr(0)
	, m_ifir(s.ifir() + offset)
	, m_ilas(s.ilas() + offset)
	, m_offset(offset)
{
	string id = m_seq.id2();

	m_ide = float(m_seq.identical()) / float(m_seq.alignment_length());
	m_wsim = float(m_seq.similar()) / float(m_seq.alignment_length());
}

Hit::~Hit()
{
	m_seq.prune();
}

struct compare_hit
{
	bool operator()(hit_ptr a, hit_ptr b) const { return *a < *b; }
};

// --------------------------------------------------------------------
// first constructor is for a 'chain-break'
ResidueHInfo::ResidueHInfo(uint32 seqNr)
	: letter(0)
	, seqNr(seqNr)
	, nocc(1)
	, ndel(0)
	, nins(0)
	, consweight(1)
{
}

ResidueHInfo::ResidueHInfo(char a, uint32 pos, char chain, uint32 seqNr, uint32 pdbNr,
		const string& dssp)
	: letter(a)
	, chain(chain)
	, dssp(dssp)
	, seqNr(seqNr)
	, pdbNr(pdbNr)
	, pos(pos)
	, nocc(1)
	, ndel(0)
	, nins(0)
	, consweight(1)
{
}

void ResidueHInfo::CalculateVariability(hit_list& hits)
{
	if (hits.empty())
		return;

	fill(dist, dist + 20, 0);
	entropy = 0;
	
	int8 ix = kResidueIX[uint8(letter)];
	if (ix >= 0)
	{
		dist[ix] = 1;
	
		foreach (hit_ptr hit, hits)
		{
			if (hit->m_chain != chain)
				continue;
	
			ix = kResidueIX[uint8(hit->m_seq[pos])];
			if (ix >= 0)
			{
				++nocc;
				dist[ix] += 1;
			}
		}

		for (uint32 a = 0; a < 20; ++a)
		{
			double freq = double(dist[a]) / nocc;
			
			dist[a] = uint32((100.0 * freq) + 0.5);
			
			if (freq > 0)
				entropy -= static_cast<float>(freq * log(freq));
		}

		// calculate ndel and nins
		const seq& q = hits.front()->m_qseq;
		
		bool gap = pos + 1 < q.length() and is_gap(q[pos + 1]);
		
		foreach (hit_ptr hit, hits)
		{
			if (hit->m_chain != chain)
				continue;
	
			const seq& t = hit->m_seq;
			
			if (pos > t.alignment_begin() and pos < t.alignment_end() and is_gap(t[pos]))
				++ndel;
			
			if (gap and t[pos] >= 'a' and t[pos] <= 'y')
				++nins;
		}
	}
}

// --------------------------------------------------------------------
// Write collected information as a HSSP file to the output stream

void CreateHSSPOutput(
	const string&		inProteinID,
	const string&		inProteinDescription,
	float				inThreshold,
	uint32				inSeqLength,
	uint32				inNChain,
	uint32				inKChain,
	const string&		inUsedChains,
	hit_list&			hits,
	res_list&			res,
	ostream&			os)
{
	using namespace boost::gregorian;
	date today = day_clock::local_day();
	
	// print the header
	os << "HSSP       HOMOLOGY DERIVED SECONDARY STRUCTURE OF PROTEINS , VERSION 2.0 2011" << endl
	   << "PDBID      " << inProteinID << endl
	   << "DATE       file generated on " << to_iso_extended_string(today) << endl
	   << "SEQBASE    " << inDatabank->GetName() << " version " << inDatabank->GetVersion() << endl
	   << "THRESHOLD  according to: t(L)=(290.15 * L ** -0.562) + " << (inThreshold * 100) << endl
	   << "REFERENCE  Sander C., Schneider R. : Database of homology-derived protein structures. Proteins, 9:56-68 (1991)." << endl
	   << "CONTACT    Maintained at http://www.cmbi.ru.nl/ by Maarten L. Hekkelman <m.hekkelman@cmbi.ru.nl>" << endl
	   << inProteinDescription
	   << boost::format("SEQLENGTH %5.5d") % inSeqLength << endl
	   << boost::format("NCHAIN     %4.4d chain(s) in %s data set") % inNChain % inProteinID << endl;
	
	if (inKChain != inNChain)
		os << boost::format("KCHAIN     %4.4d chain(s) used here ; chains(s) : ") % inKChain << inUsedChains << endl;
	
	os << boost::format("NALIGN     %4.4d") % hits.size() << endl
	   << "NOTATION : ID: EMBL/SWISSPROT identifier of the aligned (homologous) protein" << endl
	   << "NOTATION : STRID: if the 3-D structure of the aligned protein is known, then STRID is the Protein Data Bank identifier as taken" << endl
	   << "NOTATION : from the database reference or DR-line of the EMBL/SWISSPROT entry" << endl
	   << "NOTATION : %IDE: percentage of residue identity of the alignment" << endl
	   << "NOTATION : %SIM (%WSIM):  (weighted) similarity of the alignment" << endl
	   << "NOTATION : IFIR/ILAS: first and last residue of the alignment in the test sequence" << endl
	   << "NOTATION : JFIR/JLAS: first and last residue of the alignment in the alignend protein" << endl
	   << "NOTATION : LALI: length of the alignment excluding insertions and deletions" << endl
	   << "NOTATION : NGAP: number of insertions and deletions in the alignment" << endl
	   << "NOTATION : LGAP: total length of all insertions and deletions" << endl
	   << "NOTATION : LSEQ2: length of the entire sequence of the aligned protein" << endl
	   << "NOTATION : ACCNUM: SwissProt accession number" << endl
	   << "NOTATION : PROTEIN: one-line description of aligned protein" << endl
	   << "NOTATION : SeqNo,PDBNo,AA,STRUCTURE,BP1,BP2,ACC: sequential and PDB residue numbers, amino acid (lower case = Cys), secondary" << endl
	   << "NOTATION : structure, bridge partners, solvent exposure as in DSSP (Kabsch and Sander, Biopolymers 22, 2577-2637(1983)" << endl
	   << "NOTATION : VAR: sequence variability on a scale of 0-100 as derived from the NALIGN alignments" << endl
	   << "NOTATION : pair of lower case characters (AvaK) in the alignend sequence bracket a point of insertion in this sequence" << endl
	   << "NOTATION : dots (....) in the alignend sequence indicate points of deletion in this sequence" << endl
	   << "NOTATION : SEQUENCE PROFILE: relative frequency of an amino acid type at each position. Asx and Glx are in their" << endl
	   << "NOTATION : acid/amide form in proportion to their database frequencies" << endl
	   << "NOTATION : NOCC: number of aligned sequences spanning this position (including the test sequence)" << endl
	   << "NOTATION : NDEL: number of sequences with a deletion in the test protein at this position" << endl
	   << "NOTATION : NINS: number of sequences with an insertion in the test protein at this position" << endl
	   << "NOTATION : ENTROPY: entropy measure of sequence variability at this position" << endl
	   << "NOTATION : RELENT: relative entropy, i.e.  entropy normalized to the range 0-100" << endl
	   << "NOTATION : WEIGHT: conservation weight" << endl
	   << endl
	   << "## PROTEINS : identifier and alignment statistics" << endl
	   << "  NR.    ID         STRID   %IDE %WSIM IFIR ILAS JFIR JLAS LALI NGAP LGAP LSEQ2 ACCNUM     PROTEIN" << endl;
	   
	// print the first list
	uint32 nr = 1;
	boost::format fmt1("%5.5d : %12.12s%4.4s    %4.2f  %4.2f%5.5d%5.5d%5.5d%5.5d%5.5d%5.5d%5.5d%5.5d  %10.10s %s");
	foreach (hit_ptr h, hits)
	{
		const seq& s(h->m_seq);

		string id = s.id();
		if (id.length() > 12)
			id.erase(12, string::npos);
		else if (id.length() < 12)
			id.append(12 - id.length(), ' ');
		
		string acc = s.acc();
		if (acc.length() > 10)
			acc.erase(10, string::npos);
		else if (acc.length() < 10)
			acc.append(10 - acc.length(), ' ');

		string pdb = s.pdb();
		if (pdb.empty())
			pdb.append(4, ' ');
		
		os << fmt1 % nr
				   % id % pdb
				   % h->m_ide % h->m_wsim % h->m_ifir % h->m_ilas % s.jfir() % s.jlas() % s.alignment_length()
				   % s.gaps() % s.gapn() % s.lseq()
				   % acc % s.desc()
		   << endl;
		
		++nr;
	}

	// print the alignments
	for (uint32 i = 0; i < hits.size(); i += 70)
	{
		uint32 n = i + 70;
		if (n > hits.size())
			n = hits.size();
		
		uint32 k[7] = {
			((i +  0) / 10 + 1) % 10,
			((i + 10) / 10 + 1) % 10,
			((i + 20) / 10 + 1) % 10,
			((i + 30) / 10 + 1) % 10,
			((i + 40) / 10 + 1) % 10,
			((i + 50) / 10 + 1) % 10,
			((i + 60) / 10 + 1) % 10
		};
		
		os << boost::format("## ALIGNMENTS %4.4d - %4.4d") % (i + 1) % n << endl
		   << boost::format(" SeqNo  PDBNo AA STRUCTURE BP1 BP2  ACC NOCC  VAR  ....:....%1.1d....:....%1.1d....:....%1.1d....:....%1.1d....:....%1.1d....:....%1.1d....:....%1.1d")
		   					% k[0] % k[1] % k[2] % k[3] % k[4] % k[5] % k[6] << endl;

		res_ptr last;
		foreach (res_ptr ri, res)
		{
			if (ri->letter == 0)
				os << boost::format(" %5.5d        !  !           0   0    0    0    0") % ri->seqNr << endl;
			else
			{
				string aln;
				
				foreach (hit_ptr hit, boost::make_iterator_range(hits.begin() + i, hits.begin() + n))
				{
					if (ri->seqNr >= hit->m_ifir and ri->seqNr <= hit->m_ilas)
						aln += hit->m_seq[ri->pos];
					else
						aln += ' ';
				}
				
				uint32 ivar = uint32(100 * (1 - ri->consweight));

				os << ' ' << boost::format("%5.5d%s%4.4d %4.4d  ") % ri->seqNr % ri->dssp % ri->nocc % ivar << aln << endl;
			}
		}
	}
	
	// ## SEQUENCE PROFILE AND ENTROPY
	os << "## SEQUENCE PROFILE AND ENTROPY" << endl
	   << " SeqNo PDBNo   V   L   I   M   F   W   Y   G   A   P   S   T   C   H   R   K   Q   E   N   D  NOCC NDEL NINS ENTROPY RELENT WEIGHT" << endl;
	
	res_ptr last;
	foreach (res_ptr r, res)
	{
		if (r->letter == 0)
		{
			os << boost::format("%5.5d          0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0     0    0    0   0.000      0  1.00")
				% r->seqNr << endl;
		}
		else
		{
			os << boost::format("%5.5d%5.5d %c") % r->seqNr % r->pdbNr % r->chain;

			for (uint32 i = 0; i < 20; ++i)
				os << boost::format("%4.4d") % r->dist[i];

			uint32 relent = uint32(100 * r->entropy / log(20.0));
			os << "  " << boost::format("%4.4d %4.4d %4.4d   %5.3f   %4.4d  %4.2f") % r->nocc % r->ndel % r->nins % r->entropy % relent % r->consweight << endl;
		}
	}
	
	// insertion list
	
	os << "## INSERTION LIST" << endl
	   << " AliNo  IPOS  JPOS   Len Sequence" << endl;

	foreach (hit_ptr h, hits)
	{
		//foreach (insertion& ins, h->insertions)
		foreach (const insertion& ins, h->m_seq.insertions())
		{
			string s = ins.m_seq;
			
			if (s.length() <= 100)
				os << boost::format(" %5.5d %5.5d %5.5d %5.5d ") % h->m_nr % (ins.m_ipos + h->m_offset) % ins.m_jpos % (ins.m_seq.length() - 2) << s << endl;
			else
			{
				os << boost::format(" %5.5d %5.5d %5.5d %5.5d ") % h->m_nr % (ins.m_ipos + h->m_offset) % ins.m_jpos % (ins.m_seq.length() - 2) << s.substr(0, 100) << endl;
				s.erase(0, 100);
				
				while (not s.empty())
				{
					uint32 n = s.length();
					if (n > 100)
						n = 100;
					
					os << "     +                   " << s.substr(0, n) << endl;
					s.erase(0, n);
				}
			}
		}			
	}
	
	os << "//" << endl;
}

// --------------------------------------------------------------------
// Calculate the variability of a residue, based on dayhoff similarity
// and weights

uint32 kSentinel = numeric_limits<uint32>::max();
boost::mutex sSumLock;

void CalculateConservation(const mseq& msa, buffer<uint32>& b, vector<float>& csumvar, vector<float>& csumdist)
{
	const seq& s = msa.front();
	vector<float> sumvar(s.length()), sumdist(s.length()), simval(s.length());

	for (;;)
	{
		uint32 i = b.get();
		if (i == kSentinel)
			break;

		assert (msa[i].pruned() == false);

		const seq& si = msa[i];
		
		for (uint32 j = i + 1; j < msa.size(); ++j)
		{
			if (msa[j].pruned())
				continue;

			const seq& sj = msa[j];
	
			uint32 b = msa[i].alignment_begin();
			if (b < msa[j].alignment_begin())
				b = msa[j].alignment_begin();
			
			uint32 e = msa[i].alignment_end();
			if (e > msa[j].alignment_end())
				e = msa[j].alignment_end();
	
			uint32 len = 0, agr = 0;
			for (uint32 k = b; k < e; ++k)
			{
				if (not is_gap(si[k]) and not is_gap(sj[k]))
				{
					++len;
					if (si[k] == sj[k])
						++agr;

					int8 ri = kResidueIX[uint8(si[k])];
					int8 rj = kResidueIX[uint8(sj[k])];
					
					if (ri >= 0 and rj >= 0)
						simval[k] = kD(ri, rj);
					else
						simval[k] = numeric_limits<float>::min();
				}
			}

			if (len > 0)
			{
				float distance = 1 - (float(agr) / float(len));
				for (uint32 k = b; k < e; ++k)
				{
					if (simval[k] != numeric_limits<float>::min())
					{
						sumvar[k] += distance * simval[k];
						sumdist[k] += distance * 1.5f;
					}
				}
			}
		}
	}

	b.put(kSentinel);
	
	// accumulate our data
	boost::mutex::scoped_lock l(sSumLock);
	
	transform(sumvar.begin(), sumvar.end(), csumvar.begin(), csumvar.begin(), plus<float>());
	transform(sumdist.begin(), sumdist.end(), csumdist.begin(), csumdist.begin(), plus<float>());
}

void CalculateConservation(mseq& msa, boost::iterator_range<res_list::iterator>& res)
{
	if (VERBOSE)
		cerr << "Calculating conservation weights...";

	// first remove pruned seqs from msa
	//msa.erase(remove_if(msa.begin(), msa.end(), [](seq& s) { return s.m_pruned; }), msa.end());
	//msa.erase(remove_if(msa.begin(), msa.end(), boost::bind(&seq::pruned, _1)), msa.end());

	const seq& s = msa.front();
	vector<float> sumvar(s.length()), sumdist(s.length());
	
	// Calculate conservation weights in multiple threads to gain speed.
	buffer<uint32> b;
	boost::thread_group threads;
	for (uint32 t = 0; t < gNrOfThreads; ++t)
	{
		threads.create_thread(boost::bind(&CalculateConservation, boost::ref(msa),
			boost::ref(b), boost::ref(sumvar), boost::ref(sumdist)));
	}
		
	for (uint32 i = 0; i + 1 < msa.size(); ++i)
	{
		if (msa[i].pruned())
			continue;
		b.put(i);
	}
	
	b.put(kSentinel);
	threads.join_all();

	res_list::iterator ri = res.begin();
	for (uint32 i = 0; i < s.length(); ++i)
	{
		if (is_gap(s[i]))
			continue;

		float weight = 1.0f;
		if (sumdist[i] > 0)
			weight = sumvar[i] / sumdist[i];
		
		(*ri)->consweight = weight;
		
		do {
			++ri;
		} while (ri != res.end() and (*ri)->letter == 0);
	}
	assert(ri == res.end());

	if (VERBOSE)
		cerr << " done" << endl;
}

// --------------------------------------------------------------------
// Convert a multiple sequence alignment as created by jackhmmer to 
// a set of information as used by HSSP.

void ChainToHits(CDatabankPtr inDatabank, mseq& msa, const MChain& chain,
	hit_list& hits, res_list& res)
{
	if (VERBOSE)
		cerr << "Creating hits...";
	
	hit_list nhits;

	for (uint32 i = 1; i < msa.size(); ++i)
	{
		uint32 docNr;
		
		if (not inDatabank->GetDocumentNr(msa[i].id2(), docNr))
		{
			if (VERBOSE)
				cerr << "Missing document " << msa[i].id2() << endl;
			continue;
		}

		hit_ptr h(new Hit(inDatabank, msa[i], msa[0], chain.GetChainID(), res.size()));
		nhits.push_back(h);
	}
	
	if (VERBOSE)
		cerr << " done" << endl
			 << "Continuing with " << nhits.size() << " hits" << endl
			 << "Calculating residue info...";

	const vector<MResidue*>& residues = chain.GetResidues();
	vector<MResidue*>::const_iterator ri = residues.begin();

	const seq& s = msa.front();
	for (uint32 i = 0; i < s.length(); ++i)
	{
		if (is_gap(s[i]))
			continue;

		assert(ri != residues.end());
		
		if (ri != residues.begin() and (*ri)->GetNumber() > (*(ri - 1))->GetNumber() + 1)
			res.push_back(res_ptr(new ResidueHInfo(res.size() + 1)));
		
		string dssp = ResidueToDSSPLine(**ri).substr(5, 34);

		res.push_back(res_ptr(new ResidueHInfo(s[i], i,
			chain.GetChainID(), res.size() + 1, (*ri)->GetNumber(), dssp)));

		++ri;
	}
	
	if (VERBOSE)
		cerr << " done" << endl;
	
//	assert(ri == residues.end());
	hits.insert(hits.end(), nhits.begin(), nhits.end());
}

void CreateHSSP(
	CDatabankPtr		inDatabank,
	MProtein&			inProtein,
	const fs::path&		inFastaDir,
	const fs::path&		inJackHmmer,
	uint32				inIterations,
	uint32				inMaxHits,
	uint32				inMinSeqLength,
	float				inCutOff,
	ostream&			outHSSP)
{
	// construct a set of unique sequences, containing only the largest ones in case of overlap
	vector<string> seqset;
	vector<uint32> ix;
	vector<const MChain*> chains;
	
	foreach (const MChain* chain, inProtein.GetChains())
	{
		string seq;
		chain->GetSequence(seq);
		
		if (seq.length() < inMinSeqLength)
			continue;
		
		chains.push_back(chain);
		seqset.push_back(seq);
		ix.push_back(ix.size());
	}
	
	if (seqset.empty())
		THROW(("Not enough sequences in PDB file of length %d", inMinSeqLength));

	if (seqset.size() > 1)
		ClusterSequences(seqset, ix);
	
	// only take the unique sequences
	ix.erase(unique(ix.begin(), ix.end()), ix.end());

	// now create a stockholmid array
	vector<string> stockholmIds;
	
	foreach (uint32 i, ix)
	{
		const MChain* chain = chains[i];
		
		stringstream s;
		s << chain->GetChainID() << '=' << inProtein.GetID() << '-' << stockholmIds.size();
		stockholmIds.push_back(s.str());
	}
	
	CreateHSSP(inDatabank, inProtein, fs::path(), inFastaDir, inJackHmmer, inIterations, inMaxHits, stockholmIds, inCutOff, outHSSP);
}

void CreateHSSP(
	CDatabankPtr		inDatabank,
	const string&		inProtein,
	const string&		inProteinID,
	const fs::path&		inDataDir,
	const fs::path&		inFastaDir,
	const fs::path&		inJackHmmer,
	uint32				inIterations,
	uint32				inMaxHits,
	float				inCutOff,
	ostream&			outHSSP)
{
	MChain* chain = new MChain('A');
	vector<MResidue*>& residues = chain->GetResidues();
	MResidue* last = nullptr;
	uint32 nr = 1;
	foreach (char r, inProtein)
	{
		residues.push_back(new MResidue(nr, r, last));
		++nr;
		last = residues.back();
	}
	
	vector<string> stockholmIds;
	stockholmIds.push_back(string("A=") + inProteinID);
	
	MProtein protein("UNDF", chain);
	CreateHSSP(inDatabank, protein, inDataDir, inFastaDir, inJackHmmer, inIterations, inMaxHits, stockholmIds, inCutOff, outHSSP);
}

void CreateHSSP(
	CDatabankPtr		inDatabank,
	const MProtein&		inProtein,
	const fs::path&		inDataDir,
	const fs::path&		inFastaDir,
	const fs::path&		inJackHmmer,
	uint32				inIterations,
	uint32				inMaxHits,
	vector<string>		inStockholmIds,
	float				inCutOff,
	ostream&			outHSSP)
{
	uint32 seqlength = 0;

	vector<mseq> alignments(inStockholmIds.size());
	vector<const MChain*> chains;
	vector<pair<uint32,uint32> > res_ranges;

	res_list res;
	hit_list hits;

	uint32 kchain = 0;
	foreach (string ch, inStockholmIds)
	{
		if (ch.length() < 3 or ch[1] != '=')
			THROW(("Invalid chain/stockholm pair specified: '%s'", ch.c_str()));

		const MChain& chain = inProtein.GetChain(ch[0]);
		chains.push_back(&chain);

		string seq;
		chain.GetSequence(seq);

		// strip off trailing X's. They are not very useful
		while (ba::ends_with(seq, "X"))
			seq.erase(seq.end() - 1);

		if (VERBOSE > 1)
			cerr << "Chain " << ch[0] << " => '" << seq << '\'' << endl;

		seqlength += seq.length();
		
		// alignments are stored in datadir
		fs::path afp;
		if (not inDataDir.empty())
			afp = inDataDir / (ch.substr(2) + ".aln.bz2");
		if (fs::exists(afp))
		{
			fs::path afp = inDataDir / (ch.substr(2) + ".aln.bz2");

			fs::ifstream af(afp, ios::binary);
			if (not af.is_open())
				THROW(("Could not open alignment file '%s'", afp.string().c_str()));
	
			if (VERBOSE)
				cerr << "Using fasta file '" << afp << '\'' << endl;
	
			io::filtering_stream<io::input> in;
			in.push(io::bzip2_decompressor());
			in.push(af);
	
			try {
				ReadFastA(in, alignments[kchain], seq, inMaxHits);
			}
			catch (...)
			{
				cerr << "exception while reading file " << afp << endl;
				throw;
			}
		}
		else if (not inJackHmmer.empty())
		{
			try
			{
				RunJackHmmer(seq, inIterations, inFastaDir, inJackHmmer, inDatabank->GetID(),
					afp, alignments[kchain]);
				
				if (not inDataDir.empty())
				{
					fs::ofstream ff(afp, ios::binary);
					if (not ff.is_open())
						THROW(("Could not create FastA file '%s'", afp.string().c_str()));
					
					io::filtering_stream<io::output> out;
					out.push(io::bzip2_compressor());
					out.push(ff);

					WriteFastA(out, alignments[kchain]);
				}
			}
			catch (...)
			{
				cerr << "exception while running jackhmmer for chain " << chain.GetChainID() << endl;
				throw;
			}
		}
		else
			THROW(("--no-jackhmmer specified and alignment is missing, exiting"));

		// Remove all hits that are not above the threshold here
		mseq& msa = alignments[kchain];
		msa.erase(remove_if(msa.begin() + 1, msa.end(), boost::bind(&seq::drop, _1, inCutOff)), msa.end());

		++kchain;
	}

	string usedChains;
	kchain = 0;
	foreach (const MChain* chain, chains)
	{
		if (not res.empty())
			res.push_back(res_ptr(new ResidueHInfo(res.size() + 1)));
		
		uint32 first = res.size();
		
		mseq& msa = alignments[kchain];
		ChainToHits(inDatabank, msa, *chain, hits, res);
		
		res_ranges.push_back(make_pair(first, res.size()));

		if (not usedChains.empty())
			usedChains += ',';
		usedChains += chain->GetChainID();

		++kchain;
	}

	sort(hits.begin(), hits.end(), compare_hit());

	if (inMaxHits > 0 and hits.size() > inMaxHits)
		hits.erase(hits.begin() + inMaxHits, hits.end());

	if (hits.empty())
		throw mas_exception("No hits found or remaining");
	
	uint32 nr = 1;
	foreach (hit_ptr h, hits)
		h->m_nr = nr++;

	for (uint32 c = 0; c < kchain; ++c)
	{
		pair<uint32,uint32> range = res_ranges[c];
		
		res_range r(res.begin() + range.first, res.begin() + range.second);
		CalculateConservation(alignments[c], r);

		foreach (res_ptr ri, r)
			ri->CalculateVariability(hits);
	}
	
	stringstream desc;
	if (inProtein.GetHeader().length() >= 50)
		desc << "HEADER     " + inProtein.GetHeader().substr(10, 40) << endl;
	if (inProtein.GetCompound().length() > 10)
		desc << "COMPND     " + inProtein.GetCompound().substr(10) << endl;
	if (inProtein.GetSource().length() > 10)
		desc << "SOURCE     " + inProtein.GetSource().substr(10) << endl;
	if (inProtein.GetAuthor().length() > 10)
		desc << "AUTHOR     " + inProtein.GetAuthor().substr(10) << endl;

	CreateHSSPOutput(inDatabank, inProtein.GetID(), desc.str(), inCutOff, seqlength,
		inProtein.GetChains().size(), kchain, usedChains, hits, res, outHSSP);
}

void CreateHSSP(
	CDatabankPtr					inDatabank,
	std::istream&					inAlignment,
	uint32							inMaxHits,
	float							inCutOff,
	std::ostream&					outHSSP)
{
	mseq msa;
	ReadFastA(inAlignment, msa, "", inMaxHits);
	msa.erase(remove_if(msa.begin() + 1, msa.end(), boost::bind(&seq::drop, _1, inCutOff)), msa.end());

	if (msa.size() < 2)
		throw mas_exception("no alignment");

	MChain* chain = new MChain('A');
	vector<MResidue*>& residues = chain->GetResidues();
	MResidue* last = nullptr;
	uint32 nr = 1;
	foreach (char r, msa.front())
	{
		if (is_gap(r))
			continue;

		residues.push_back(new MResidue(nr, r, last));
		++nr;
		last = residues.back();
	}

	MProtein protein("UNDF", chain);

	res_list res;
	hit_list hits;

	ChainToHits(inDatabank, msa, *chain, hits, res);

	sort(hits.begin(), hits.end(), compare_hit());

	if (inMaxHits > 0 and hits.size() > inMaxHits)
		hits.erase(hits.begin() + inMaxHits, hits.end());

	if (hits.empty())
		throw mas_exception("No hits found or remaining");
	
	nr = 1;
	foreach (hit_ptr h, hits)
		h->m_nr = nr++;

	res_range r(res.begin(), res.end());
	CalculateConservation(msa, r);

	foreach (res_ptr ri, r)
		ri->CalculateVariability(hits);
	
	stringstream desc;
	if (protein.GetHeader().length() >= 50)
		desc << "HEADER     " + protein.GetHeader().substr(10, 40) << endl;
	if (protein.GetCompound().length() > 10)
		desc << "COMPND     " + protein.GetCompound().substr(10) << endl;
	if (protein.GetSource().length() > 10)
		desc << "SOURCE     " + protein.GetSource().substr(10) << endl;
	if (protein.GetAuthor().length() > 10)
		desc << "AUTHOR     " + protein.GetAuthor().substr(10) << endl;

	CreateHSSPOutput(inDatabank, protein.GetID(), desc.str(), inCutOff, res.size(),
		protein.GetChains().size(), 1, "A", hits, res, outHSSP);
}

void ConvertHsspFile(
	const fs::path&	inHssp3File,
	const fs::path&	inHssp1File)
{
	fs::ifstream sf(inHssp3File, ios::binary);
	if (not sf.is_open())
		THROW(("Could not open input file '%s'", inHssp3File.string().c_str()));

	io::filtering_stream<io::input> in;
	if (inHssp3File.extension() == ".bz2")
		in.push(io::bzip2_decompressor());
	else if (inHssp3File.extension() == ".gz")
		in.push(io::gzip_decompressor());
	in.push(sf);

	for (;;)
	{
		
		


		uint32 seqlength = 0;
	
		vector<mseq> alignments(inStockholmIds.size());
		vector<const MChain*> chains;
		vector<pair<uint32,uint32> > res_ranges;
	
		res_list res;
		hit_list hits;
	
		uint32 kchain = 0;
		foreach (string ch, inStockholmIds)
		{
			if (ch.length() < 3 or ch[1] != '=')
				THROW(("Invalid chain/stockholm pair specified: '%s'", ch.c_str()));
	
			const MChain& chain = inProtein.GetChain(ch[0]);
			chains.push_back(&chain);
	
			string seq;
			chain.GetSequence(seq);
	
			// strip off trailing X's. They are not very useful
			while (ba::ends_with(seq, "X"))
				seq.erase(seq.end() - 1);
	
			if (VERBOSE > 1)
				cerr << "Chain " << ch[0] << " => '" << seq << '\'' << endl;
	
			seqlength += seq.length();
			
			// alignments are stored in datadir
			fs::path afp;
			if (not inDataDir.empty())
				afp = inDataDir / (ch.substr(2) + ".aln.bz2");
			if (fs::exists(afp))
			{
				fs::path afp = inDataDir / (ch.substr(2) + ".aln.bz2");
	
				fs::ifstream af(afp, ios::binary);
				if (not af.is_open())
					THROW(("Could not open alignment file '%s'", afp.string().c_str()));
		
				if (VERBOSE)
					cerr << "Using fasta file '" << afp << '\'' << endl;
		
				io::filtering_stream<io::input> in;
				in.push(io::bzip2_decompressor());
				in.push(af);
		
				try {
					ReadFastA(in, alignments[kchain], seq, inMaxHits);
				}
				catch (...)
				{
					cerr << "exception while reading file " << afp << endl;
					throw;
				}
			}
			else if (not inJackHmmer.empty())
			{
				try
				{
					RunJackHmmer(seq, inIterations, inFastaDir, inJackHmmer, inDatabank->GetID(),
						afp, alignments[kchain]);
					
					if (not inDataDir.empty())
					{
						fs::ofstream ff(afp, ios::binary);
						if (not ff.is_open())
							THROW(("Could not create FastA file '%s'", afp.string().c_str()));
						
						io::filtering_stream<io::output> out;
						out.push(io::bzip2_compressor());
						out.push(ff);
	
						WriteFastA(out, alignments[kchain]);
					}
				}
				catch (...)
				{
					cerr << "exception while running jackhmmer for chain " << chain.GetChainID() << endl;
					throw;
				}
			}
			else
				THROW(("--no-jackhmmer specified and alignment is missing, exiting"));
	
			// Remove all hits that are not above the threshold here
			mseq& msa = alignments[kchain];
			msa.erase(remove_if(msa.begin() + 1, msa.end(), boost::bind(&seq::drop, _1, inCutOff)), msa.end());
	
			++kchain;
		}
	
		string usedChains;
		kchain = 0;
		foreach (const MChain* chain, chains)
		{
			if (not res.empty())
				res.push_back(res_ptr(new ResidueHInfo(res.size() + 1)));
			
			uint32 first = res.size();
			
			mseq& msa = alignments[kchain];
			ChainToHits(inDatabank, msa, *chain, hits, res);
			
			res_ranges.push_back(make_pair(first, res.size()));
	
			if (not usedChains.empty())
				usedChains += ',';
			usedChains += chain->GetChainID();
	
			++kchain;
		}
	
		sort(hits.begin(), hits.end(), compare_hit());
	
		if (inMaxHits > 0 and hits.size() > inMaxHits)
			hits.erase(hits.begin() + inMaxHits, hits.end());
	
		if (hits.empty())
			throw mas_exception("No hits found or remaining");
		
		uint32 nr = 1;
		foreach (hit_ptr h, hits)
			h->m_nr = nr++;
	
		for (uint32 c = 0; c < kchain; ++c)
		{
			pair<uint32,uint32> range = res_ranges[c];
			
			res_range r(res.begin() + range.first, res.begin() + range.second);
			CalculateConservation(alignments[c], r);
	
			foreach (res_ptr ri, r)
				ri->CalculateVariability(hits);
		}
		
		stringstream desc;
		if (inProtein.GetHeader().length() >= 50)
			desc << "HEADER     " + inProtein.GetHeader().substr(10, 40) << endl;
		if (inProtein.GetCompound().length() > 10)
			desc << "COMPND     " + inProtein.GetCompound().substr(10) << endl;
		if (inProtein.GetSource().length() > 10)
			desc << "SOURCE     " + inProtein.GetSource().substr(10) << endl;
		if (inProtein.GetAuthor().length() > 10)
			desc << "AUTHOR     " + inProtein.GetAuthor().substr(10) << endl;
	
	fs::ofstream ff(inHssp1File, ios::binary);
	if (not ff.is_open())
		THROW(("Could not create output file '%s'", inHssp1File.string().c_str()));
	
	io::filtering_stream<io::output> out;
	if (inHssp1File.extension() == ".bz2")
		out.push(io::bzip2_compressor());
	else if (inHssp1File.extension() == ".gz")
		out.push(io::gzip_compressor());
	out.push(ff);
	
	CreateHSSPOutput(inDatabank, inProtein.GetID(), desc.str(), inCutOff, seqlength,
		inProtein.GetChains().size(), kchain, usedChains, hits, res, out);
}

}


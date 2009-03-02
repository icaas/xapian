/** @file image_terms.cc
 * @brief Generate terms from an image signature.
 */
/* Copyright 2009 Lemur Consulting Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301
 * USA
 */

#include <config.h>

#include "xapian/error.h"
#include "xapian/imgseek.h"

#include "haar.h" // For num_pixels_squared - FIXME - should be supplied to ImgTerms constructor.
#include "serialise-double.h"

// Declarations of functions in other file: FIXME - should have a shared internal header.
double find_weight(const int idx, const int colour);
const float weights[6][3]= {
  //    Y      I      Q       idx total occurs
  { 5.00, 19.21, 34.37},  // 0   58.58      1 (`DC' component)
  { 0.83,  1.26,  0.36},  // 1    2.45      3
  { 1.01,  0.44,  0.45},  // 2    1.90      5
  { 0.52,  0.53,  0.14},  // 3    1.19      7
  { 0.47,  0.28,  0.18},  // 4    0.93      9
  { 0.30,  0.14,  0.27}   // 5    0.71      16384-25=16359
};


template<class T>
inline std::string to_string(const T& t) {
    std::stringstream ss;
    ss << t;
    return ss.str();
}

std::string
ImgTerms::colourprefix(int c) const {
    return prefix + to_string(c);
}

std::string 
ImgTerms::make_coeff_term(int x, int c) const {
    return colourprefix(c) + to_string(x);
}

WeightMap
ImgTerms::make_weightmap() const {
    WeightMap wm;
    for (int x = -num_pixels_squared; x < num_pixels_squared; ++x) {
	for (int c=0; c< 3; c++) {
	    wm[make_coeff_term(x, c)] = find_weight(x, c);
	}
    }
    return wm;
}

/* Ranges for the Y, I, and Q values in the YIQ colourspace. */
#define Y_MIN 0.0
#define Y_MAX 1.0
#define I_MIN -0.523
#define I_MAX 0.523
#define Q_MIN -0.596
#define Q_MAX 0.596

ImgTerms::ImgTerms(const std::string& prefix_,
		   Xapian::valueno v1,
		   Xapian::valueno v2,
		   Xapian::valueno v3)
	: prefix(prefix_),
	  weightmap(make_weightmap())
{
    colour_vals.push_back(v1);
    colour_vals.push_back(v2);
    colour_vals.push_back(v3);

    // Put the values into 255 buckets.

    // Y - ranges from 0.0 to 1.0
    colour_average_accels.push_back(
	Xapian::RangeAccelerator(prefix + "A0",
				 colour_vals[0],
				 Y_MIN, Y_MAX,
				 (Y_MAX - Y_MIN) / 255.0));
    // I - ranges from -0.523 to 0.523
    colour_average_accels.push_back(
	Xapian::RangeAccelerator(prefix + "A1",
				 colour_vals[1],
				 I_MIN, I_MAX,
				 (I_MAX - I_MIN) / 255.0));
    // Q - ranges from -0.596 to 0.596
    colour_average_accels.push_back(
	Xapian::RangeAccelerator(prefix + "A2",
				 colour_vals[2],
				 Q_MIN, Q_MAX,
				 (Q_MAX - Q_MIN) / 255.0));
}

void 
ImgTerms::add_coeff_terms(const coeffs& s, int c, CoeffTerms& r) const {
    coeffs::const_iterator it;
    for (it = s.begin(); it != s.end(); ++it)
	r.insert(make_coeff_term(*it, c));
}
      
CoeffTerms
ImgTerms::make_coeff_terms(const ImgSig& sig) const {
    CoeffTerms terms;
    add_coeff_terms(sig.sig1, 0, terms);
    add_coeff_terms(sig.sig2, 1, terms);
    add_coeff_terms(sig.sig3, 2, terms);
    return terms;
}

void
ImgTerms::AddTerms(Xapian::Document& doc, const ImgSig& sig) const {
    CoeffTerms terms = make_coeff_terms(sig);
    CoeffTerms::const_iterator it;
    for (it = terms.begin(); it != terms.end(); ++it) {
	doc.add_term(*it);
    }
    for (int c = 0; c < 3; ++c) {
	colour_average_accels[c].add_val(doc, sig.avgl[c]);
    }
}

// this could be faster - it checks the whole string
bool 
startswith(const std::string& s, const std::string& start){
    return s.find(start) == 0;
}

Xapian::Query::Query 
ImgTerms::make_coeff_query(const Xapian::Document& doc) const {
    Xapian::Query::Query query;
    Xapian::TermIterator it;
    for (int c = 0; c < 3; ++c) {
	it = doc.termlist_begin();
	std::string cprefix = colourprefix(c);
	it.skip_to(cprefix);
	while (it != doc.termlist_end() && startswith(*it, cprefix)) {
	    Xapian::Query::Query subq = Xapian::Query(*it);
	    WeightMap::const_iterator pos = weightmap.find(*it);
	    subq = Xapian::Query(Xapian::Query::OP_SCALE_WEIGHT,
				 subq,
				 pos->second);
	    query = Xapian::Query(Xapian::Query::OP_OR, 
				  query,
				  subq);
	    ++it;
	} 
    }
    return query;
}

Xapian::Query 
ImgTerms::querySimilar(const Xapian::Document& doc) const {
    return Xapian::Query(Xapian::Query::OP_OR,
			 make_coeff_query(doc),
			 make_averages_query(doc));
}

Xapian::Query
ImgTerms::make_averages_query(const Xapian::Document& doc) const {
    Xapian::Query query;
    for (int c = 0; c < 3; ++c) {
	std::string doc_val = doc.get_value(colour_vals[c]);
	const char* ptr = doc_val.data();
	const char* end = ptr + doc_val.size();
	double val;
	try {
	    val = unserialise_double(&ptr, end);
	} catch (const Xapian::NetworkError & e) {
	    throw Xapian::InvalidArgumentError(e.get_msg());
	}
	Xapian::Query subq = Xapian::Query(Xapian::Query::OP_SCALE_WEIGHT, 
					   colour_average_accels[c].query_for_val_distance(val),
					   weights[0][c]);
	query = Xapian::Query(Xapian::Query::OP_OR,
			      query,
			      subq);
    }
    return query;
}

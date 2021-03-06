#include "PCNTools.h"

#include <iostream>

namespace PCN
{

const std::string chars = "'\\";
const char& quote = chars[0];
const char& slash = chars[1];

// safe get
inline char get(const std::string& in, int c) {
	if (c < 0 || c >= (int)in.size()) return 0;
	else return in[(size_t)c];
}

// consume whitespace
inline void eatws(const std::string& in, int& c) {
	while (get(in,c) == ' ') { c++; }
}

// from 'foo' return foo
std::string getEscapedString(const std::string& in, int &c)
{
	eatws(in,c);
	if (get(in,c++) != quote) return "ERROR";
	std::string res;
	char cur = 0;
	do {
		cur = get(in,c++);
		if (cur == slash) { res += get(in,c++); }
		else if (cur != quote) { res += cur; }
	} while (get(in,c) != quote && (c < (int)in.size()));
	c++;
	eatws(in,c);
	return res;
}

// basically atof
float getFloat(const std::string& in, int &c)
{
	std::string tmp;
	eatws(in,c);
	while (c < (int)in.size() && get(in,c) != ' ' && get(in,c) != ')' && get(in,c) != ',') {
		tmp += get(in,c++);
	}
	eatws(in,c);
	return atof(tmp.c_str());
}

// basically atof
int getInt(const std::string& in, int &c)
{
	std::string tmp;
	eatws(in,c);
	while (c < (int)in.size() && get(in,c) != ' ' && get(in,c) != ')' && get(in,c) != ',') {
		tmp += get(in,c++);
	}
	eatws(in,c);
	return atoi(tmp.c_str());
}

// parse ('foo', 0.23)
CNAlt getCNAlt(const std::string& in, int &c)
{
	if (get(in,c++) != '(') { std::cerr << "PCN/PLF parse error: expected ( at start of cn alt block\n"; return CNAlt(); } // throw "expected (";
	std::string word = getEscapedString(in,c);
	if (get(in,c++) != ',') { std::cerr << "PCN/PLF parse error: expected , after string\n"; return CNAlt(); } // throw "expected , after string";
	size_t cnNext = 1;
	std::vector<float> probs;
	probs.push_back(getFloat(in,c));
	while (get(in,c) == ',') {
		c++;
		float val = getFloat(in,c);
		probs.push_back(val);
	}
	//if we read more than one prob, this was a lattice, last item was column increment
	if (probs.size()>1) {
		cnNext = static_cast<size_t>(probs.back());
		probs.pop_back();
		if (cnNext < 1) { ; std::cerr << "PCN/PLF parse error: bad link length at last element of cn alt block\n"; return CNAlt(); } //throw "bad link length"
	}
	if (get(in,c++) != ')') { std::cerr << "PCN/PLF parse error: expected ) at end of cn alt block\n"; return CNAlt(); } // throw "expected )";
	eatws(in,c);
	return CNAlt(std::pair<std::string, std::vector<float> >(word,probs), cnNext);
}

// parse (('foo', 0.23), ('bar', 0.77))
CNCol getCNCol(const std::string& in, int &c) {
	CNCol res;
	if (get(in,c++) != '(') return res;  // error
	eatws(in,c);
	while (1) {
		if (c > (int)in.size()) { break; }
		if (get(in,c) == ')') {
			c++;
			eatws(in,c);
			break;
		}
		if (get(in,c) == ',' && get(in,c+1) == ')') {
			c+=2;
			eatws(in,c);
			break;
		}
		if (get(in,c) == ',') { c++; eatws(in,c); }
		res.push_back(getCNAlt(in, c));
	}
	return res;
}

// parse ((('foo', 0.23), ('bar', 0.77)), (('a', 0.3), ('c', 0.7)))
CN parsePCN(const std::string& in)
{
	CN res;
	int c = 0;
	if (in[c++] != '(') return res; // error
	while (1) {
		if (c > (int)in.size()) { break; }
		if (get(in,c) == ')') {
			c++;
			eatws(in,c);
			break;
		}
		if (get(in,c) == ',' && get(in,c+1) == ')') {
			c+=2;
			eatws(in,c);
			break;
		}
		if (get(in,c) == ',') { c++; eatws(in,c); }
		res.push_back(getCNCol(in, c));
	}
	return res;
}

}


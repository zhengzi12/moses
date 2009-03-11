// $Id$
// vim:tabstop=2
/***********************************************************************
Moses - factored phrase-based language decoder
Copyright (C) 2006 University of Edinburgh

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
***********************************************************************/

#include <cassert>
#include <iostream>
#include <limits>
#include <vector>
#include <algorithm>
#include "TranslationOption.h"
#include "TranslationOptionCollection.h"
#include "DummyScoreProducers.h"
#include "Hypothesis.h"
#include "Util.h"
#include "SquareMatrix.h"
#include "LexicalReordering.h"
#include "StaticData.h"
#include "InputType.h"
#include "LMList.h"
#include "hash.h"

using namespace std;

namespace Moses
{
unsigned int Hypothesis::s_HypothesesCreated = 0;

#ifdef USE_HYPO_POOL
	ObjectPool<Hypothesis> Hypothesis::s_objectPool("Hypothesis", 300000);
#endif

Hypothesis::Hypothesis(InputType const& source, const TargetPhrase &emptyTarget)
	: m_prevHypo(NULL)
	, m_transOpt(NULL)
	, m_targetPhrase(emptyTarget)
	, m_sourcePhrase(0)
	, m_sourceCompleted(source.GetSize())
	, m_sourceInput(source)
	, m_currSourceWordsRange(NOT_FOUND, NOT_FOUND)
	, m_currTargetWordsRange(NOT_FOUND, NOT_FOUND)
	, m_wordDeleted(false)
	, m_languageModelStates(StaticData::Instance().GetLMSize(), LanguageModelSingleFactor::UnknownState)
	, m_arcList(NULL)
	, m_id(0)
  , m_lmstats(NULL)
  , m_numSpans(1)
  //  , m_alignPair(source.GetSize())
{	// used for initial seeding of trans process	
	// initialize scores
	//_hash_computed = false;
	s_HypothesesCreated = 1;
	ResetScore();	
}

/***
 * continue prevHypo by appending the phrases in transOpt
 */
Hypothesis::Hypothesis(const Hypothesis &prevHypo, const TranslationOption &transOpt)
	: m_prevHypo(&prevHypo)
	, m_transOpt(&transOpt)
	, m_targetPhrase(transOpt.GetTargetPhrase())
	, m_sourcePhrase(transOpt.GetSourcePhrase())
	, m_sourceCompleted				(prevHypo.m_sourceCompleted )
	, m_sourceInput						(prevHypo.m_sourceInput)
	, m_currSourceWordsRange	(transOpt.GetSourceWordsRange())
	, m_currTargetWordsRange	( prevHypo.m_currTargetWordsRange.GetEndPos() + 1
														 ,prevHypo.m_currTargetWordsRange.GetEndPos() + transOpt.GetTargetPhrase().GetSize())
	, m_wordDeleted(false)
	,	m_totalScore(0.0f)
	,	m_futureScore(0.0f)
	, m_scoreBreakdown				(prevHypo.m_scoreBreakdown)
	, m_languageModelStates(prevHypo.m_languageModelStates)
	, m_arcList(NULL)
	, m_id(s_HypothesesCreated++)
  , m_lmstats(NULL)
  , m_numSpans(prevHypo.m_numSpans)
//  , m_alignPair(prevHypo.m_alignPair)
{
	// assert that we are not extending our hypothesis by retranslating something
	// that this hypothesis has already translated!
	assert(!m_sourceCompleted.Overlap(m_currSourceWordsRange));	

	//_hash_computed = false;
  m_sourceCompleted.SetValue(m_currSourceWordsRange.GetStartPos(), m_currSourceWordsRange.GetEndPos(), true);
  m_wordDeleted = transOpt.IsDeletionOption();
	m_scoreBreakdown.PlusEquals(transOpt.GetScoreBreakdown());
}

Hypothesis::~Hypothesis()
{
	if (m_arcList) 
	{
		ArcList::iterator iter;
		for (iter = m_arcList->begin() ; iter != m_arcList->end() ; ++iter)
		{
			FREEHYPO(*iter);
		}
		m_arcList->clear();

		delete m_arcList;
		m_arcList = NULL;

		delete m_lmstats; m_lmstats = NULL;
	}
}

void Hypothesis::AddArc(Hypothesis *loserHypo)
{
	if (!m_arcList) {
		if (loserHypo->m_arcList)  // we don't have an arcList, but loser does
		{
			this->m_arcList = loserHypo->m_arcList;  // take ownership, we'll delete
			loserHypo->m_arcList = 0;                // prevent a double deletion
		}
		else
			{ this->m_arcList = new ArcList(); }
	} else {
		if (loserHypo->m_arcList) {  // both have an arc list: merge. delete loser
			size_t my_size = m_arcList->size();
			size_t add_size = loserHypo->m_arcList->size();
			this->m_arcList->resize(my_size + add_size, 0);
			std::memcpy(&(*m_arcList)[0] + my_size, &(*loserHypo->m_arcList)[0], add_size * sizeof(Hypothesis *));
			delete loserHypo->m_arcList;
			loserHypo->m_arcList = 0;
		} else { // loserHypo doesn't have any arcs
		  // DO NOTHING
		}
	}
	m_arcList->push_back(loserHypo);
}

/***
 * return the subclass of Hypothesis most appropriate to the given translation option
 */
Hypothesis* Hypothesis::CreateNext(const TranslationOption &transOpt, const Phrase* constraint) const
{
	return Create(*this, transOpt, constraint);
}

/***
 * return the subclass of Hypothesis most appropriate to the given translation option
 */
Hypothesis* Hypothesis::Create(const Hypothesis &prevHypo, const TranslationOption &transOpt, const Phrase* constrainingPhrase)
{

	// This method includes code for constraint decoding
	
	bool createHypothesis = true;

	if (constrainingPhrase != NULL)
	{

		size_t constraintSize = constrainingPhrase->GetSize();
			
		size_t start = 1 + prevHypo.GetCurrTargetWordsRange().GetEndPos();
			
		const Phrase &transOptPhrase = transOpt.GetTargetPhrase();
		size_t transOptSize = transOptPhrase.GetSize();
		
		size_t endpoint = start + transOptSize - 1;
		

		if (endpoint < constraintSize) 
		{	
			WordsRange range(start, endpoint);
			Phrase relevantConstraint = constrainingPhrase->GetSubString(range);
			
			if ( ! relevantConstraint.IsCompatible(transOptPhrase) )
			{
				createHypothesis = false;
				
			}
		}
		else 
		{
			createHypothesis = false;
		}
		
	}

	
	if (createHypothesis)
	{

		#ifdef USE_HYPO_POOL
			Hypothesis *ptr = s_objectPool.getPtr();
			return new(ptr) Hypothesis(prevHypo, transOpt);
		#else
			return new Hypothesis(prevHypo, transOpt);
		#endif

	}
	else
	{
		// If the previous hypothesis plus the proposed translation option
		//    fail to match the provided constraint,
		//    return a null hypothesis.
		return NULL;
	}
	
}
/***
 * return the subclass of Hypothesis most appropriate to the given target phrase
 */

Hypothesis* Hypothesis::Create(InputType const& m_source, const TargetPhrase &emptyTarget)
{
#ifdef USE_HYPO_POOL
	Hypothesis *ptr = s_objectPool.getPtr();
	return new(ptr) Hypothesis(m_source, emptyTarget);
#else
	return new Hypothesis(m_source, emptyTarget);
#endif
}

/** check, if two hypothesis can be recombined.
    this is actually a sorting function that allows us to
    keep an ordered list of hypotheses. This makes recombination
    much quicker. 
*/
int Hypothesis::NGramCompare(const Hypothesis &compare) const
{ // -1 = this < compare
	// +1 = this > compare
	// 0	= this ==compare
	if (m_languageModelStates < compare.m_languageModelStates) return -1;
	if (m_languageModelStates > compare.m_languageModelStates) return 1;

	int compareBitmap = m_sourceCompleted.Compare(compare.m_sourceCompleted);
	if (compareBitmap != 0)
		return compareBitmap;
	if (m_currSourceWordsRange.GetEndPos() < compare.m_currSourceWordsRange.GetEndPos()) return -1;
	if (m_currSourceWordsRange.GetEndPos() > compare.m_currSourceWordsRange.GetEndPos()) return 1;
	if (! StaticData::Instance().GetSourceStartPosMattersForRecombination()) return 0;
	if (m_currSourceWordsRange.GetStartPos() < compare.m_currSourceWordsRange.GetStartPos()) return -1;
	if (m_currSourceWordsRange.GetStartPos() > compare.m_currSourceWordsRange.GetStartPos()) return 1;
	return 0;
}

/** Calculates the overall language model score by combining the scores
 * of language models generated for each of the factors.  Because the factors
 * represent a variety of tag sets, and because factors with smaller tag sets 
 * (such as POS instead of words) allow us to calculate richer statistics, we
 * allow a different length of n-gram to be specified for each factor.
 * /param lmListInitial todo - describe this parameter 
 * /param lmListEnd todo - describe this parameter
 */
void Hypothesis::CalcLMScore(const LMList &languageModels)
{
	LMList::const_iterator iterLM;
	clock_t t=0;
	IFVERBOSE(2) { t  = clock(); } // track time
	const size_t startPos	= m_currTargetWordsRange.GetStartPos();

	// will be null if LM stats collection is disabled
	if (StaticData::Instance().IsComputeLMBackoffStats()) {
		m_lmstats = new vector<vector<unsigned int> >(languageModels.size(), vector<unsigned int>(0));
	}

	size_t lmIdx = 0;

	// already have LM scores from previous and trigram score of poss trans.
	// just need n-gram score of the words of the start of current phrase	
	for (iterLM = languageModels.begin() ; iterLM != languageModels.end() ; ++iterLM,++lmIdx)
	{
		const LanguageModel &languageModel = **iterLM;
		size_t nGramOrder			= languageModel.GetNGramOrder();
		size_t currEndPos			= m_currTargetWordsRange.GetEndPos();
		float lmScore;
		size_t nLmCallCount = 0;

		if(m_currTargetWordsRange.GetNumWordsCovered() == 0) {
			lmScore = 0; //the score associated with dropping source words is not part of the language model
		} else { //non-empty target phrase
			if (m_lmstats)
				(*m_lmstats)[lmIdx].resize(m_currTargetWordsRange.GetNumWordsCovered(), 0);

			// 1st n-gram
			vector<const Word*> contextFactor(nGramOrder);
			size_t index = 0;
			for (int currPos = (int) startPos - (int) nGramOrder + 1 ; currPos <= (int) startPos ; currPos++)
			{
				if (currPos >= 0)
					contextFactor[index++] = &GetWord(currPos);
				else			
					contextFactor[index++] = &languageModel.GetSentenceStartArray();
			}
			lmScore	= languageModel.GetValue(contextFactor);
			if (m_lmstats) { languageModel.GetState(contextFactor, &(*m_lmstats)[lmIdx][nLmCallCount++]); }
			//cout<<"context factor: "<<languageModel.GetValue(contextFactor)<<endl;

			// main loop
			size_t endPos = std::min(startPos + nGramOrder - 2
															, currEndPos);
			for (size_t currPos = startPos + 1 ; currPos <= endPos ; currPos++)
			{
				// shift all args down 1 place
				for (size_t i = 0 ; i < nGramOrder - 1 ; i++)
					contextFactor[i] = contextFactor[i + 1];
	
				// add last factor
				contextFactor.back() = &GetWord(currPos);

				lmScore	+= languageModel.GetValue(contextFactor);
				if (m_lmstats) 
					languageModel.GetState(contextFactor, &(*m_lmstats)[lmIdx][nLmCallCount++]);
				//cout<<"context factor: "<<languageModel.GetValue(contextFactor)<<endl;		
			}

			// end of sentence
			if (m_sourceCompleted.IsComplete())
			{
				const size_t size = GetSize();
				contextFactor.back() = &languageModel.GetSentenceEndArray();
	
				for (size_t i = 0 ; i < nGramOrder - 1 ; i ++)
				{
					int currPos = (int)(size - nGramOrder + i + 1);
					if (currPos < 0)
						contextFactor[i] = &languageModel.GetSentenceStartArray();
					else
						contextFactor[i] = &GetWord((size_t)currPos);
				}
				if (m_lmstats) {
					(*m_lmstats)[lmIdx].resize((*m_lmstats)[lmIdx].size() + 1); // extra space for the last call
					lmScore += languageModel.GetValue(contextFactor, &m_languageModelStates[lmIdx], &(*m_lmstats)[lmIdx][nLmCallCount++]);
				} else
					lmScore	+= languageModel.GetValue(contextFactor, &m_languageModelStates[lmIdx]);
			} else {
				for (size_t currPos = endPos+1; currPos <= currEndPos; currPos++) {
					for (size_t i = 0 ; i < nGramOrder - 1 ; i++)
						contextFactor[i] = contextFactor[i + 1];
					contextFactor.back() = &GetWord(currPos);
					if (m_lmstats)
						languageModel.GetState(contextFactor, &(*m_lmstats)[lmIdx][nLmCallCount++]);
				}
				m_languageModelStates[lmIdx]=languageModel.GetState(contextFactor);
			}
		}
		
		m_scoreBreakdown.PlusEquals(&languageModel, lmScore);
	}
	IFVERBOSE(2) { StaticData::Instance().GetSentenceStats().AddTimeCalcLM( clock()-t ); }
}

void Hypothesis::CalcDistortionScore()

{
	const DistortionScoreProducer *dsp = StaticData::Instance().GetDistortionScoreProducer();
	float distortionScore = dsp->CalculateDistortionScore(
			m_prevHypo->GetCurrSourceWordsRange(),
			this->GetCurrSourceWordsRange(),
			m_prevHypo->GetWordsBitmap().GetFirstGapPos()
     );
	m_scoreBreakdown.PlusEquals(dsp, distortionScore);
}

void Hypothesis::ResetScore()
{
	m_scoreBreakdown.ZeroAll();
	m_futureScore = m_totalScore = 0.0f;
}

/***
 * calculate the logarithm of our total translation score (sum up components)
 */
void Hypothesis::CalcScore(const SquareMatrix &futureScore) 
{
	const StaticData &staticData = StaticData::Instance();
	clock_t t=0; // used to track time

	// LANGUAGE MODEL COST
	CalcLMScore(staticData.GetAllLM());

	IFVERBOSE(2) { t = clock(); } // track time excluding LM

	// DISTORTION COST
	CalcDistortionScore();
	
	// WORD PENALTY
	m_scoreBreakdown.PlusEquals(staticData.GetWordPenaltyProducer(), - (float) m_currTargetWordsRange.GetNumWordsCovered()); 

	// FUTURE COST
	m_futureScore = futureScore.CalcFutureScore( m_sourceCompleted );
	
	//LEXICAL REORDERING COST
	const std::vector<LexicalReordering*> &reorderModels = staticData.GetReorderModels();
	for(unsigned int i = 0; i < reorderModels.size(); i++)
	{
		m_scoreBreakdown.PlusEquals(reorderModels[i], reorderModels[i]->CalcScore(this));
	}

	// TOTAL
	m_totalScore = m_scoreBreakdown.InnerProduct(staticData.GetAllWeights()) + m_futureScore;

	IFVERBOSE(2) { staticData.GetSentenceStats().AddTimeOtherScore( clock()-t ); }
}

/** Calculates the expected score of extending this hypothesis with the
 * specified translation option. Includes actual costs for everything 
 * except for expensive actual language model score.
 * This function is used by early discarding.
 * /param transOpt - translation option being considered
 */
float Hypothesis::CalcExpectedScore( const SquareMatrix &futureScore ) {
	const StaticData &staticData = StaticData::Instance();
	clock_t t=0;
	IFVERBOSE(2) { t = clock(); } // track time excluding LM

	// DISTORTION COST
	CalcDistortionScore();
	
	// LANGUAGE MODEL ESTIMATE (includes word penalty cost)
  float estimatedLMScore = m_transOpt->GetFutureScore() - m_transOpt->GetScoreBreakdown().InnerProduct(staticData.GetAllWeights());

	// FUTURE COST
	m_futureScore = futureScore.CalcFutureScore( m_sourceCompleted );

	//LEXICAL REORDERING COST
	const std::vector<LexicalReordering*> &reorderModels = staticData.GetReorderModels();
	for(unsigned int i = 0; i < reorderModels.size(); i++)
	{
		m_scoreBreakdown.PlusEquals(reorderModels[i], reorderModels[i]->CalcScore(this));
	}

	// TOTAL
	float total = m_scoreBreakdown.InnerProduct(staticData.GetAllWeights()) + m_futureScore + estimatedLMScore;

	IFVERBOSE(2) { staticData.GetSentenceStats().AddTimeEstimateScore( clock()-t ); }
	return total;
}

void Hypothesis::CalcRemainingScore() 
{
	const StaticData &staticData = StaticData::Instance();
	clock_t t=0; // used to track time

	// LANGUAGE MODEL COST
	CalcLMScore(staticData.GetAllLM());

	IFVERBOSE(2) { t = clock(); } // track time excluding LM

	// WORD PENALTY
	m_scoreBreakdown.PlusEquals(staticData.GetWordPenaltyProducer(), - (float) m_currTargetWordsRange.GetNumWordsCovered()); 

	// TOTAL
	m_totalScore = m_scoreBreakdown.InnerProduct(staticData.GetAllWeights()) + m_futureScore;

	IFVERBOSE(2) { StaticData::Instance().GetSentenceStats().AddTimeOtherScore( clock()-t ); }
}

const Hypothesis* Hypothesis::GetPrevHypo()const{
	return m_prevHypo;
}

const Hypothesis* Hypothesis::GetNextHypo() const {
  return m_nextHypo;
}

const Hypothesis* Hypothesis::GetSourcePrevHypo()const{
  return m_sourcePrevHypo;
}

const Hypothesis* Hypothesis::GetSourceNextHypo() const {
  return m_sourceNextHypo;
}

/**
 * print hypothesis information for pharaoh-style logging
 */
void Hypothesis::PrintHypothesis() const
{
  if (!m_prevHypo) { TRACE_ERR(endl << "NULL hypo" << endl); return; }
  TRACE_ERR(endl << "creating hypothesis "<< m_id <<" from "<< m_prevHypo->m_id<<" ( ");
  int end = (int)(m_prevHypo->m_targetPhrase.GetSize()-1);
  int start = end-1;
  if ( start < 0 ) start = 0;
  if ( m_prevHypo->m_currTargetWordsRange.GetStartPos() == NOT_FOUND ) {
    TRACE_ERR( "<s> ");
  }
  else {
    TRACE_ERR( "... ");
  }
  if (end>=0) {
    WordsRange range(start, end);
    TRACE_ERR( m_prevHypo->m_targetPhrase.GetSubString(range) << " ");
  }
  TRACE_ERR( ")"<<endl);
	TRACE_ERR( "\tbase score "<< (m_prevHypo->m_totalScore - m_prevHypo->m_futureScore) <<endl);
	TRACE_ERR( "\tcovering "<<m_currSourceWordsRange.GetStartPos()<<"-"<<m_currSourceWordsRange.GetEndPos()<<": "
	  << *m_sourcePhrase <<endl);
	TRACE_ERR( "\ttranslated as: "<<(Phrase&) m_targetPhrase<<endl); // <<" => translation cost "<<m_score[ScoreType::PhraseTrans];
	if (PrintAlignmentInfo()){
		TRACE_ERR( "\tsource-target word alignment: "<< m_targetPhrase.GetAlignmentPair().GetAlignmentPhrase(Input) << endl); // <<" => source to target word-to-word alignment
		TRACE_ERR( "\ttarget-source word alignment: "<< m_targetPhrase.GetAlignmentPair().GetAlignmentPhrase(Output) << endl); // <<" => target to source word-to-word alignment
	}
	if (m_wordDeleted) TRACE_ERR( "\tword deleted"<<endl); 
  //	TRACE_ERR( "\tdistance: "<<GetCurrSourceWordsRange().CalcDistortion(m_prevHypo->GetCurrSourceWordsRange())); // << " => distortion cost "<<(m_score[ScoreType::Distortion]*weightDistortion)<<endl;
  //	TRACE_ERR( "\tlanguage model cost "); // <<m_score[ScoreType::LanguageModelScore]<<endl;
  //	TRACE_ERR( "\tword penalty "); // <<(m_score[ScoreType::WordPenalty]*weightWordPenalty)<<endl;
	TRACE_ERR( "\tscore "<<m_totalScore - m_futureScore<<" + future cost "<<m_futureScore<<" = "<<m_totalScore<<endl);
  TRACE_ERR(  "\tunweighted feature scores: " << m_scoreBreakdown << endl);
	//PrintLMScores();
}

void Hypothesis::CleanupArcList()
{
	// point this hypo's main hypo to itself
	SetWinningHypo(this);

	if (!m_arcList) return;

	/* keep only number of arcs we need to create all n-best paths.
	 * However, may not be enough if only unique candidates are needed,
	 * so we'll keep all of arc list if nedd distinct n-best list
	 */
	const StaticData &staticData = StaticData::Instance();
	size_t nBestSize = staticData.GetNBestSize();
	bool distinctNBest = staticData.GetDistinctNBest() || staticData.UseMBR() || staticData.GetOutputSearchGraph();

	if (!distinctNBest && m_arcList->size() > nBestSize * 5)
	{ // prune arc list only if there too many arcs
		nth_element(m_arcList->begin()
							, m_arcList->begin() + nBestSize - 1
							, m_arcList->end()
							, CompareHypothesisTotalScore());
		
		// delete bad ones
		ArcList::iterator iter;
		for (iter = m_arcList->begin() + nBestSize ; iter != m_arcList->end() ; ++iter)
		{
			Hypothesis *arc = *iter;
			FREEHYPO(arc);
		}
		m_arcList->erase(m_arcList->begin() + nBestSize
										, m_arcList->end());
	}

	// set all arc's main hypo variable to this hypo
	ArcList::iterator iter = m_arcList->begin();
	for (; iter != m_arcList->end() ; ++iter)
	{
		Hypothesis *arc = *iter;
		arc->SetWinningHypo(this);
	}
}

TO_STRING_BODY(Hypothesis)
 
// friend
ostream& operator<<(ostream& out, const Hypothesis& hypothesis)
{	
	hypothesis.ToStream(out);
	// words bitmap
	out << "[" << hypothesis.m_sourceCompleted << "] ";
	
	// scores
	out << " [total=" << hypothesis.GetTotalScore() << "]";
	out << " " << hypothesis.GetScoreBreakdown();
	
	// alignment
	if (hypothesis.PrintAlignmentInfo()){
		out << " [f2e:";
		hypothesis.SourceAlignmentToStream(out);
		out << "]";
		out << " [e2f:";
		hypothesis.TargetAlignmentToStream(out);
		out << "]";
	}
	return out;
}


std::string Hypothesis::GetSourcePhraseStringRep(const vector<FactorType> factorsToPrint) const 
{
	if (!m_prevHypo) { return ""; }
	return m_sourcePhrase->GetStringRep(factorsToPrint);
#if 0
	if(m_sourcePhrase) 
	{
		return m_sourcePhrase->GetSubString(m_currSourceWordsRange).GetStringRep(factorsToPrint);
	}
	else
	{ 
		return m_sourceInput.GetSubString(m_currSourceWordsRange).GetStringRep(factorsToPrint);
	}	
#endif
}
std::string Hypothesis::GetTargetPhraseStringRep(const vector<FactorType> factorsToPrint) const 
{
	if (!m_prevHypo) { return ""; }
	return m_targetPhrase.GetStringRep(factorsToPrint);
}

std::string Hypothesis::GetSourcePhraseStringRep() const 
{
	vector<FactorType> allFactors;
	const size_t maxSourceFactors = StaticData::Instance().GetMaxNumFactors(Input);
	for(size_t i=0; i < maxSourceFactors; i++)
	{
		allFactors.push_back(i);
	}
	return GetSourcePhraseStringRep(allFactors);		
}
std::string Hypothesis::GetTargetPhraseStringRep() const 
{
	vector<FactorType> allFactors;
	const size_t maxTargetFactors = StaticData::Instance().GetMaxNumFactors(Output);
	for(size_t i=0; i < maxTargetFactors; i++)
	{
		allFactors.push_back(i);
	}
	return GetTargetPhraseStringRep(allFactors);
}


const ScoreComponentCollection &Hypothesis::GetCachedReorderingScore() const
{
	return m_transOpt->GetReorderingScore();
}

void Hypothesis::GetTranslation(std::vector<const Factor*>* trans, const FactorType ft) const {
	if (m_prevHypo) m_prevHypo->GetTranslation(trans, ft);
	for (unsigned i = 0; i < m_targetPhrase.GetSize(); ++i)
		trans->push_back(m_targetPhrase.GetFactor(i, ft));
}

}


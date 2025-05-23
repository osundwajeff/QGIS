/*
 *   libpal - Automated Placement of Labels Library
 *
 *   Copyright (C) 2008 Maxence Laurent, MIS-TIC, HEIG-VD
 *                      University of Applied Sciences, Western Switzerland
 *                      http://www.hes-so.ch
 *
 *   Contact:
 *      maxence.laurent <at> heig-vd <dot> ch
 *    or
 *      eric.taillard <at> heig-vd <dot> ch
 *
 * This file is part of libpal.
 *
 * libpal is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libpal is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libpal.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "pal.h"
#include "layer.h"
#include "feature.h"
#include "geomfunction.h"
#include "labelposition.h"
#include "problem.h"
#include "util.h"
#include "priorityqueue.h"
#include "internalexception.h"
#include "qgslabelingenginerule.h"
#include <cfloat>
#include <limits> //for std::numeric_limits<int>::max()

#include "qgslabelingengine.h"

using namespace pal;

inline void delete_chain( Chain *chain )
{
  if ( chain )
  {
    delete[] chain->feat;
    delete[] chain->label;
    delete chain;
  }
}

Problem::Problem( const QgsRectangle &extent )
  : mAllCandidatesIndex( extent )
  , mActiveCandidatesIndex( extent )
{

}

void Problem::addCandidatePosition( std::unique_ptr<LabelPosition> position )
{
  mLabelPositions.emplace_back( std::move( position ) );
}

Problem::~Problem() = default;

void Problem::reduce()
{
  int k;

  int counter = 0;

  int lpid;

  std::vector<bool> ok( mTotalCandidates, false );

  LabelPosition *lp2 = nullptr;

  while ( true )
  {
    if ( pal->isCanceled() )
      break;

    bool finished = true;
    for ( std::size_t feature = 0; feature < mFeatureCount; feature++ )
    {
      if ( pal->isCanceled() )
        break;

      // for each candidate
      const int totalCandidatesForFeature = mCandidateCountForFeature[feature];
      for ( int candidateIndex = 0; candidateIndex < totalCandidatesForFeature; candidateIndex++ )
      {
        if ( !ok[mFirstCandidateIndexForFeature[feature] + candidateIndex] )
        {
          if ( mLabelPositions.at( mFirstCandidateIndexForFeature[feature] + candidateIndex )->getNumOverlaps() == 0 ) // if candidate has no overlap
          {
            finished = false;
            ok[mFirstCandidateIndexForFeature[feature] + candidateIndex] = true;
            // 1) remove worse candidates from candidates
            // 2) update nb_overlaps
            counter += totalCandidatesForFeature - candidateIndex - 1;

            for ( k = candidateIndex + 1; k < totalCandidatesForFeature; k++ )
            {

              lpid = mFirstCandidateIndexForFeature[feature] + k;
              ok[lpid] = true;
              lp2 = mLabelPositions[lpid ].get();

              mNbOverlap -= lp2->getNumOverlaps();

              const QgsRectangle searchBounds = lp2->boundingBoxForCandidateConflicts( pal );
              mAllCandidatesIndex.intersects( searchBounds, [&lp2, this]( const LabelPosition * lp ) -> bool
              {
                if ( candidatesAreConflicting( lp2, lp ) )
                {
                  const_cast< LabelPosition * >( lp )->decrementNumOverlaps();
                  lp2->decrementNumOverlaps();
                }

                return true;
              } );
              lp2->removeFromIndex( mAllCandidatesIndex, pal );
            }

            mCandidateCountForFeature[feature] = candidateIndex + 1;
            break;
          }
        }
      }
    }

    if ( finished )
      break;
  }

  this->mTotalCandidates -= counter;
}

void Problem::ignoreLabel( const LabelPosition *lp, PriorityQueue &list, PalRtree< LabelPosition > &candidatesIndex )
{
  if ( list.isIn( lp->getId() ) )
  {
    list.remove( lp->getId() );

    const QgsRectangle searchBounds = lp->boundingBoxForCandidateConflicts( pal );
    candidatesIndex.intersects( searchBounds, [lp, &list, this]( const LabelPosition * lp2 )->bool
    {
      if ( lp2->getId() != lp->getId() && list.isIn( lp2->getId() ) && candidatesAreConflicting( lp2, lp ) )
      {
        list.decreaseKey( lp2->getId() );
      }
      return true;
    } );
  }
}

/* Better initial solution
 * Step one FALP (Yamamoto, Camara, Lorena 2005)
 */
void Problem::init_sol_falp()
{
  int label;

  mSol.init( mFeatureCount );

  PriorityQueue list( mTotalCandidates, mAllNblp, true );

  LabelPosition *lp = nullptr;

  for ( int feature = 0; feature < static_cast< int >( mFeatureCount ); feature++ )
  {
    const int totalCandidatesForFeature = mCandidateCountForFeature[feature];
    for ( int candidateIndex = 0; candidateIndex < totalCandidatesForFeature; candidateIndex++ )
    {
      label = mFirstCandidateIndexForFeature[feature] + candidateIndex;
      try
      {
        list.insert( label, mLabelPositions.at( label )->getNumOverlaps() );
      }
      catch ( pal::InternalException::Full & )
      {
        continue;
      }
    }
  }

  while ( list.getSize() > 0 ) // O (log size)
  {
    if ( pal->isCanceled() )
    {
      return;
    }

    label = list.getBest();   // O (log size)

    lp = mLabelPositions[ label ].get();

    if ( lp->getId() != label )
    {
      //error
    }

    const int probFeatId = lp->getProblemFeatureId();
    mSol.activeLabelIds[probFeatId] = label;

    for ( int candidateIndex = mFirstCandidateIndexForFeature[probFeatId]; candidateIndex < mFirstCandidateIndexForFeature[probFeatId] + mCandidateCountForFeature[probFeatId]; candidateIndex++ )
    {
      ignoreLabel( mLabelPositions[ candidateIndex ].get(), list, mAllCandidatesIndex );
    }


    const QgsRectangle searchBounds = lp->boundingBoxForCandidateConflicts( pal );
    std::vector< const LabelPosition * > conflictingPositions;
    mAllCandidatesIndex.intersects( searchBounds, [lp, &conflictingPositions, this]( const LabelPosition * lp2 ) ->bool
    {
      if ( candidatesAreConflicting( lp, lp2 ) )
      {
        conflictingPositions.emplace_back( lp2 );
      }
      return true;
    } );

    for ( const LabelPosition *conflict : conflictingPositions )
    {
      ignoreLabel( conflict, list, mAllCandidatesIndex );
    }

    lp->insertIntoIndex( mActiveCandidatesIndex, pal );
  }

  if ( mDisplayAll )
  {
    LabelPosition *retainedLabel = nullptr;

    for ( std::size_t i = 0; i < mFeatureCount; i++ ) // forearch hidden feature
    {
      if ( mSol.activeLabelIds[i] == -1 )
      {
        int nbOverlap = std::numeric_limits<int>::max();
        const int firstCandidateIdForFeature = mFirstCandidateIndexForFeature[i];
        const int totalCandidatesForFeature = mCandidateCountForFeature[i];
        for ( int candidateIndexForFeature = 0; candidateIndexForFeature < totalCandidatesForFeature; candidateIndexForFeature++ )
        {
          lp = mLabelPositions[ firstCandidateIdForFeature + candidateIndexForFeature ].get();
          lp->resetNumOverlaps();

          const QgsRectangle searchBounds = lp->boundingBoxForCandidateConflicts( pal );
          mActiveCandidatesIndex.intersects( searchBounds, [&lp, this]( const LabelPosition * lp2 )->bool
          {
            if ( candidatesAreConflicting( lp, lp2 ) )
            {
              lp->incrementNumOverlaps();
            }
            return true;
          } );

          if ( lp->getNumOverlaps() < nbOverlap )
          {
            retainedLabel = lp;
            nbOverlap = lp->getNumOverlaps();
          }
        }
        mSol.activeLabelIds[i] = retainedLabel->getId();

        retainedLabel->insertIntoIndex( mActiveCandidatesIndex, pal );

      }
    }
  }
}

bool Problem::candidatesAreConflicting( const LabelPosition *lp1, const LabelPosition *lp2 ) const
{
  return pal->candidatesAreConflicting( lp1, lp2 );
}

inline Chain *Problem::chain( int seed )
{
  int lid;

  double delta;
  double delta_min;
  double delta_best = std::numeric_limits<double>::max();
  double delta_tmp;

  int next_seed;
  int retainedLabel;
  Chain *retainedChain = nullptr;

  const int max_degree = pal->mEjChainDeg;

  QLinkedList<ElemTrans *> currentChain;
  QLinkedList<int> conflicts;

  std::vector< int > tmpsol( mSol.activeLabelIds );

  LabelPosition *lp = nullptr;

  // delta is actually related to the cost?
  delta = 0;
  // seed is actually the feature number!
  while ( seed != -1 )
  {
    const int totalCandidatesForThisFeature = mCandidateCountForFeature[seed];
    delta_min = std::numeric_limits<double>::max();

    next_seed = -1;
    retainedLabel = -2;

    // sol[seed] is ejected
    if ( tmpsol[seed] == -1 )
      delta -= mUnlabeledCostForFeature[seed];
    else
      delta -= mLabelPositions.at( tmpsol[seed] )->cost();

    for ( int i = -1; i < totalCandidatesForThisFeature ; i++ )
    {
      try
      {
        // Skip active label !
        if ( !( tmpsol[seed] == -1 && i == -1 ) && i + mFirstCandidateIndexForFeature[seed] != tmpsol[seed] )
        {
          if ( i != -1 ) // new_label
          {
            lid = mFirstCandidateIndexForFeature[seed] + i;
            delta_tmp = delta;

            lp = mLabelPositions[ lid ].get();

            const QgsRectangle searchBounds = lp->boundingBoxForCandidateConflicts( pal );
            // evaluate conflicts graph in solution after moving seed's label
            mActiveCandidatesIndex.intersects( searchBounds, [lp, &delta_tmp, &conflicts, &currentChain, this]( const LabelPosition * lp2 ) -> bool
            {
              if ( candidatesAreConflicting( lp2, lp ) )
              {
                const int feat = lp2->getProblemFeatureId();

                // is there any cycles ?
                QLinkedList< ElemTrans * >::iterator cur;
                for ( cur = currentChain.begin(); cur != currentChain.end(); ++cur )
                {
                  if ( ( *cur )->feat == feat )
                  {
                    throw - 1;
                  }
                }

                if ( !conflicts.contains( feat ) )
                {
                  conflicts.append( feat );
                  delta_tmp += lp2->cost() + mUnlabeledCostForFeature[feat];
                }
              }
              return true;
            } );

            // no conflict -> end of chain
            if ( conflicts.isEmpty() )
            {
              if ( !retainedChain || delta + lp->cost() < delta_best )
              {
                if ( retainedChain )
                {
                  delete[] retainedChain->label;
                  delete[] retainedChain->feat;
                }
                else
                {
                  retainedChain = new Chain();
                }

                delta_best = delta + lp->cost();

                retainedChain->degree = currentChain.size() + 1;
                retainedChain->feat  = new int[retainedChain->degree];
                retainedChain->label = new int[retainedChain->degree];
                QLinkedList<ElemTrans *>::iterator current = currentChain.begin();
                ElemTrans *move = nullptr;
                int j = 0;
                while ( current != currentChain.end() )
                {
                  move = *current;
                  retainedChain->feat[j]  = move->feat;
                  retainedChain->label[j] = move->new_label;
                  ++current;
                  ++j;
                }
                retainedChain->feat[j] = seed;
                retainedChain->label[j] = lid;
                retainedChain->delta = delta + lp->cost();
              }
            }

            // another feature can be ejected
            else if ( conflicts.size() == 1 )
            {
              if ( delta_tmp < delta_min )
              {
                delta_min = delta_tmp;
                retainedLabel = lid;
                next_seed = conflicts.takeFirst();
              }
              else
              {
                conflicts.takeFirst();
              }
            }
            else
            {

              // A lot of conflict : make them inactive and store chain
              Chain *newChain = new Chain();
              newChain->degree = currentChain.size() + 1 + conflicts.size();
              newChain->feat  = new int[newChain->degree];
              newChain->label = new int[newChain->degree];
              QLinkedList<ElemTrans *>::iterator current = currentChain.begin();
              ElemTrans *move = nullptr;
              int j = 0;

              while ( current != currentChain.end() )
              {
                move = *current;
                newChain->feat[j]  = move->feat;
                newChain->label[j] = move->new_label;
                ++current;
                ++j;
              }

              // add the current candidates into the chain
              newChain->feat[j] = seed;
              newChain->label[j] = lid;
              newChain->delta = delta + mLabelPositions.at( newChain->label[j] )->cost();
              j++;

              // hide all conflictual candidates
              while ( !conflicts.isEmpty() )
              {
                const int ftid = conflicts.takeFirst();
                newChain->feat[j] = ftid;
                newChain->label[j] = -1;
                newChain->delta += mUnlabeledCostForFeature[ftid];
                j++;
              }

              if ( newChain->delta < delta_best )
              {
                if ( retainedChain )
                  delete_chain( retainedChain );

                delta_best = newChain->delta;
                retainedChain = newChain;
              }
              else
              {
                delete_chain( newChain );
              }
            }

          }
          else   // Current label == -1   end of chain ...
          {
            if ( !retainedChain || delta + mUnlabeledCostForFeature[seed] < delta_best )
            {
              if ( retainedChain )
              {
                delete[] retainedChain->label;
                delete[] retainedChain->feat;
              }
              else
                retainedChain = new Chain();

              delta_best = delta + mUnlabeledCostForFeature[seed];

              retainedChain->degree = currentChain.size() + 1;
              retainedChain->feat  = new int[retainedChain->degree];
              retainedChain->label = new int[retainedChain->degree];
              QLinkedList<ElemTrans *>::iterator current = currentChain.begin();
              ElemTrans *move = nullptr;
              int j = 0;
              while ( current != currentChain.end() )
              {
                move = *current;
                retainedChain->feat[j]  = move->feat;
                retainedChain->label[j] = move->new_label;
                ++current;
                ++j;
              }
              retainedChain->feat[j] = seed;
              retainedChain->label[j] = -1;
              retainedChain->delta = delta + mUnlabeledCostForFeature[seed];
            }
          }
        }
      }
      catch ( int )
      {
        conflicts.clear();
      }
    } // end for each labelposition

    if ( next_seed == -1 )
    {
      seed = -1;
    }
    else if ( currentChain.size() > max_degree )
    {
      // Max degree reached
      seed = -1;
    }
    else
    {
      ElemTrans *et = new ElemTrans();
      et->feat  = seed;
      et->old_label = tmpsol[seed];
      et->new_label = retainedLabel;
      currentChain.append( et );

      if ( et->old_label != -1 )
      {
        mLabelPositions.at( et->old_label )->removeFromIndex( mActiveCandidatesIndex, pal );
      }

      if ( et->new_label != -1 )
      {
        mLabelPositions.at( et->new_label )->insertIntoIndex( mActiveCandidatesIndex, pal );
      }


      tmpsol[seed] = retainedLabel;
      // cppcheck-suppress invalidFunctionArg
      delta += mLabelPositions.at( retainedLabel )->cost();
      seed = next_seed;
    }
  }

  while ( !currentChain.isEmpty() )
  {
    std::unique_ptr< ElemTrans > et( currentChain.takeFirst() );

    if ( et->new_label != -1 )
    {
      mLabelPositions.at( et->new_label )->removeFromIndex( mActiveCandidatesIndex, pal );
    }

    if ( et->old_label != -1 )
    {
      mLabelPositions.at( et->old_label )->insertIntoIndex( mActiveCandidatesIndex, pal );
    }
  }

  return retainedChain;
}


void Problem::chainSearch( QgsRenderContext & )
{
  if ( mFeatureCount == 0 )
    return;

  int i;
  bool *ok = new bool[mFeatureCount];
  int fid;
  int lid;

  Chain *retainedChain = nullptr;

  std::fill( ok, ok + mFeatureCount, false );

  init_sol_falp();

  int iter = 0;

  // seed is actually the feature ID, maybe should be renamed?
  int seed;
  while ( true )
  {
    for ( seed = ( iter + 1 ) % mFeatureCount;
          ok[seed] && seed != iter;
          seed = ( seed + 1 ) % mFeatureCount )
      ;

    // All seeds are OK
    if ( seed == iter )
    {
      break;
    }

    iter = ( iter + 1 ) % mFeatureCount;
    retainedChain = chain( seed );

    if ( retainedChain && retainedChain->delta < - EPSILON )
    {
      // apply modification
      for ( i = 0; i < retainedChain->degree; i++ )
      {
        fid = retainedChain->feat[i];
        lid = retainedChain->label[i];

        if ( mSol.activeLabelIds[fid] >= 0 )
        {
          LabelPosition *old = mLabelPositions[ mSol.activeLabelIds[fid] ].get();
          old->removeFromIndex( mActiveCandidatesIndex, pal );

          const QgsRectangle searchBounds = old->boundingBoxForCandidateConflicts( pal );
          mAllCandidatesIndex.intersects( searchBounds, [&ok, old, this]( const LabelPosition * lp ) ->bool
          {
            if ( candidatesAreConflicting( old, lp ) )
            {
              ok[lp->getProblemFeatureId()] = false;
            }

            return true;
          } );
        }

        mSol.activeLabelIds[fid] = lid;

        if ( mSol.activeLabelIds[fid] >= 0 )
        {
          mLabelPositions.at( lid )->insertIntoIndex( mActiveCandidatesIndex, pal );
        }

        ok[fid] = false;
      }
    }
    else
    {
      // no chain or the one is not good enough
      ok[seed] = true;
    }

    delete_chain( retainedChain );
  }

  delete[] ok;
}

QList<LabelPosition *> Problem::getSolution( bool returnInactive, QList<LabelPosition *> *unlabeled )
{
  QList<LabelPosition *> finalLabelPlacements;
  finalLabelPlacements.reserve( mFeatureCount );

  // loop through all features to be labeled
  for ( std::size_t i = 0; i < mFeatureCount; i++ )
  {
    const int labelId = mSol.activeLabelIds[i];
    const bool foundNonOverlappingPlacement = labelId != -1;
    const int startIndexForLabelPlacements = mFirstCandidateIndexForFeature[i];
    const bool foundCandidatesForFeature = startIndexForLabelPlacements < static_cast< int >( mLabelPositions.size() );

    if ( foundNonOverlappingPlacement )
    {
      finalLabelPlacements.push_back( mLabelPositions[ labelId ].get() ); // active labels
    }
    else if ( foundCandidatesForFeature &&
              ( returnInactive // allowing any overlapping labels regardless of where they are from
                || mLabelPositions.at( startIndexForLabelPlacements )->getFeaturePart()->feature()->overlapHandling() == Qgis::LabelOverlapHandling::AllowOverlapIfRequired // allowing overlapping labels for the layer
                || mLabelPositions.at( startIndexForLabelPlacements )->getFeaturePart()->alwaysShow() ) ) // allowing overlapping labels for the feature
    {
      finalLabelPlacements.push_back( mLabelPositions[ startIndexForLabelPlacements ].get() ); // unplaced label
    }
    else if ( unlabeled )
    {
      // need to be careful here -- if the next feature's start id is the same as this one, then this feature had no candidates!
      if ( foundCandidatesForFeature && ( i == mFeatureCount - 1 || startIndexForLabelPlacements != mFirstCandidateIndexForFeature[i + 1] ) )
        unlabeled->push_back( mLabelPositions[ startIndexForLabelPlacements ].get() );
    }
  }

  // unlabeled features also include those with no candidates
  if ( unlabeled )
  {
    unlabeled->reserve( mPositionsWithNoCandidates.size() );
    for ( const std::unique_ptr< LabelPosition > &position : mPositionsWithNoCandidates )
      unlabeled->append( position.get() );
  }

  return finalLabelPlacements;
}

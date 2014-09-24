//
//   Copyright 2014 DreamWorks Animation LLC.
//
//   Licensed under the Apache License, Version 2.0 (the "Apache License")
//   with the following modification; you may not use this file except in
//   compliance with the Apache License and the following modification to it:
//   Section 6. Trademarks. is deleted and replaced with:
//
//   6. Trademarks. This License does not grant permission to use the trade
//      names, trademarks, service marks, or product names of the Licensor
//      and its affiliates, except as required to comply with Section 4(c) of
//      the License and to reproduce the content of the NOTICE file.
//
//   You may obtain a copy of the Apache License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the Apache License with the above modification is
//   distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
//   KIND, either express or implied. See the Apache License for the specific
//   language governing permissions and limitations under the Apache License.
//
#include "../far/topologyRefiner.h"
#include "../vtr/sparseSelector.h"

#include <cassert>
#include <cstdio>


namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Far {

//
//  Relatively trivial construction/destruction -- the base level (level[0]) needs
//  to be explicitly initialized after construction and refinement then applied
//
TopologyRefiner::TopologyRefiner(Sdc::Type schemeType, Sdc::Options schemeOptions) :
    _subdivType(schemeType),
    _subdivOptions(schemeOptions),
    _isUniform(true),
    _maxLevel(0) {

    //  Need to revisit allocation scheme here -- want to use smart-ptrs for these
    //  but will probably have to settle for explicit new/delete...
    _levels.reserve(8);
    _levels.resize(1);
}

TopologyRefiner::~TopologyRefiner() { }

void
TopologyRefiner::Unrefine() {
    if (_levels.size()) {
        _levels.resize(1);
    }
    _refinements.clear();
}

void
TopologyRefiner::Clear() {
    _levels.clear();
    _refinements.clear();
}


//
//  Accessors to the topology information:
//
int
TopologyRefiner::GetNumVerticesTotal() const {
    int sum = 0;
    for (int i = 0; i < (int)_levels.size(); ++i) {
        sum += _levels[i].getNumVertices();
    }
    return sum;
}
int
TopologyRefiner::GetNumEdgesTotal() const {
    int sum = 0;
    for (int i = 0; i < (int)_levels.size(); ++i) {
        sum += _levels[i].getNumEdges();
    }
    return sum;
}
int
TopologyRefiner::GetNumFacesTotal() const {
    int sum = 0;
    for (int i = 0; i < (int)_levels.size(); ++i) {
        sum += _levels[i].getNumFaces();
    }
    return sum;
}
int
TopologyRefiner::GetNumFaceVerticesTotal() const {
    int sum = 0;
    for (int i = 0; i < (int)_levels.size(); ++i) {
        sum += _levels[i].getNumFaceVerticesTotal();
    }
    return sum;
}
int
TopologyRefiner::GetNumFVarValuesTotal(int channel) const {
    int sum = 0;
    for (int i = 0; i < (int)_levels.size(); ++i) {
        sum += _levels[i].getNumFVarValues(channel);
    }
    return sum;
}


template <Sdc::Type SCHEME_TYPE> void
computePtexIndices(Vtr::Level const & coarseLevel, std::vector<int> & ptexIndices) {
    int nfaces = coarseLevel.getNumFaces();
    ptexIndices.resize(nfaces+1);
    int ptexID=0;
    for (int i = 0; i < nfaces; ++i) {
        ptexIndices[i] = ptexID;
        Vtr::IndexArray fverts = coarseLevel.getFaceVertices(i);
        ptexID += fverts.size()==Sdc::TypeTraits<SCHEME_TYPE>::RegularFaceValence() ? 1 : fverts.size();
    }
    // last entry contains the number of ptex texture faces
    ptexIndices[nfaces]=ptexID;
}
void
TopologyRefiner::initializePtexIndices() const {
    std::vector<int> & indices = const_cast<std::vector<int> &>(_ptexIndices);
    switch (GetSchemeType()) {
        case Sdc::TYPE_BILINEAR:
            computePtexIndices<Sdc::TYPE_BILINEAR>(_levels[0], indices); break;
        case Sdc::TYPE_CATMARK :
            computePtexIndices<Sdc::TYPE_CATMARK>(_levels[0], indices); break;
        case Sdc::TYPE_LOOP    :
            computePtexIndices<Sdc::TYPE_LOOP>(_levels[0], indices); break;
    }
}
int
TopologyRefiner::GetNumPtexFaces() const {
    if (_ptexIndices.empty()) {
        initializePtexIndices();
    }
    // see computePtexIndices()
    return _ptexIndices.back();
}
int
TopologyRefiner::GetPtexIndex(Index f) const {
    if (_ptexIndices.empty()) {
        initializePtexIndices();
    }
    if (f<((int)_ptexIndices.size()-1)) {
        return _ptexIndices[f];
    }
    return -1;
}


//
//  Main refinement method -- allocating and initializing levels and refinements:
//
void
TopologyRefiner::RefineUniform(int maxLevel, bool fullTopology) {

    assert(_levels[0].getNumVertices() > 0);  //  Make sure the base level has been initialized
    assert(_subdivType == Sdc::TYPE_CATMARK);

    //
    //  Allocate the stack of levels and the refinements between them:
    //
    _isUniform = true;
    _maxLevel = maxLevel;

    _levels.resize(maxLevel + 1);
    _refinements.resize(maxLevel);

    //
    //  Initialize refinement options for Vtr -- adjusting full-topology for the last level:
    //
    Vtr::Refinement::Options refineOptions;
    refineOptions._sparse = false;

    for (int i = 1; i <= maxLevel; ++i) {
        refineOptions._faceTopologyOnly = fullTopology ? false : (i == maxLevel);

        _refinements[i-1].setScheme(_subdivType, _subdivOptions);
        _refinements[i-1].initialize(_levels[i-1], _levels[i]);
        _refinements[i-1].refine(refineOptions);
    }
}


void
TopologyRefiner::RefineAdaptive(int subdivLevel, bool fullTopology) {

    assert(_levels[0].getNumVertices() > 0);  //  Make sure the base level has been initialized
    assert(_subdivType == Sdc::TYPE_CATMARK);

    //
    //  Allocate the stack of levels and the refinements between them:
    //
    _isUniform = false;
    _maxLevel = subdivLevel;

    //  Should we presize all or grow one at a time as needed?
    _levels.resize(subdivLevel + 1);
    _refinements.resize(subdivLevel);

    //
    //  Initialize refinement options for Vtr:
    //
    Vtr::Refinement::Options refineOptions;

    refineOptions._sparse           = true;
    refineOptions._faceTopologyOnly = !fullTopology;

    for (int i = 1; i <= subdivLevel; ++i) {
        //  Keeping full topology on for debugging -- may need to go back a level and "prune"
        //  its topology if we don't use the full depth
        refineOptions._faceTopologyOnly = false;

        Vtr::Level& parentLevel     = _levels[i-1];
        Vtr::Level& childLevel      = _levels[i];
        Vtr::Refinement& refinement = _refinements[i-1];

        refinement.setScheme(_subdivType, _subdivOptions);
        refinement.initialize(parentLevel, childLevel);

        //
        //  Initialize a Selector to mark a sparse set of components for refinement.  Refine
        //  if something was selected, otherwise terminate refinement and trim the Level and
        //  Refinement vectors to remove the curent refinement and child that were in progress:
        //
        Vtr::SparseSelector selector(refinement);

        //  Scheme-specific methods may become part of the Selector...
        catmarkFeatureAdaptiveSelector(selector);

        if (!selector.isSelectionEmpty()) {
            refinement.refine(refineOptions);

            //childLevel.print(&refinement);
            //assert(childLevel.validateTopology());
        } else {
            //  Note that if we support the "full topology at last level" option properly,
            //  we should prune the previous level generated, as it is now the last...
            int maxLevel = i - 1;

            _maxLevel = maxLevel;
            _levels.resize(maxLevel + 1);
            _refinements.resize(maxLevel);
            break;
        }
    }
}

//
//   Catmark-specific method for feature-adaptive selection for sparse refinement at each level.
//
//   It assumes we have a freshly initialized Vtr::SparseSelector (i.e. nothing already selected)
//   and will select all relevant topological features for inclusion in the subsequent sparse
//   refinement.
//
//   With appropriate topological tags on the components, i.e. which vertices are extra-ordinary,
//   non-manifold, etc., there's no reason why this can't be written in a way that is independent
//   of the subdivision scheme.  All of the creasing cases are independent, leaving only the
//   regularity associated with the scheme.
//
void
TopologyRefiner::catmarkFeatureAdaptiveSelector(Vtr::SparseSelector& selector) {

    Vtr::Level const& level = selector.getRefinement().parent();

    for (Vtr::Index face = 0; face < level.getNumFaces(); ++face) {
        Vtr::IndexArray const faceVerts = level.getFaceVertices(face);

        //
        //  Testing irregular faces is only necessary at level 0, and potentially warrants
        //  separating out as the caller can detect these (and generically as long as we
        //  can identify an irregular face for all schemes):
        //
        if (faceVerts.size() != 4) {
            //  
            //  We need to also ensure that all adjacent faces to this are selected, so we
            //  select every face incident every vertex of the face.  This is the only place
            //  where other faces are selected as a side effect and somewhat undermines the
            //  whole intent of the per-face traversal.
            //
            Vtr::IndexArray const fVerts = level.getFaceVertices(face);
            for (int i = 0; i < fVerts.size(); ++i) {
                IndexArray const fVertFaces = level.getVertexFaces(fVerts[i]);
                for (int j = 0; j < fVertFaces.size(); ++j) {
                    selector.selectFace(fVertFaces[j]);
                }
            }
            continue;
        }

        //
        //  Combine the tags for all vertices of the face and quickly accept/reject based on
        //  the presence/absence of properties where we can (further inspection is likely to
        //  be necessary in some cases, particularly when we start trying to be clever about
        //  minimizing refinement for inf-sharp creases, etc.):
        //
        Vtr::Level::VTag compFaceTag = level.getFaceCompositeVTag(faceVerts);
        if (compFaceTag._incomplete) {
            continue;
        }

        bool selectFace = false;
        if (compFaceTag._xordinary || compFaceTag._semiSharp) {
            selectFace = true;
        } else if (compFaceTag._rule & Sdc::Crease::RULE_DART) {
            //  Get this case out of the way before testing hard features
            selectFace = true;
        } else if (compFaceTag._nonManifold) {
            //  Warrants further inspection -- isolate for now
            //    - will want to defer inf-sharp treatment to below
            selectFace = true;
        } else if (!(compFaceTag._rule & Sdc::Crease::RULE_SMOOTH)) {
            //  None of the vertices is Smooth, so we have all vertices either Crease or Corner,
            //  though some may be regular patches, this currently warrants isolation as we only
            //  support regular patches with one corner or one boundary.
            selectFace = true;
        } else {
            //  This leaves us with at least one Smooth vertex (and so two smooth adjacent edges
            //  of the quad) and the rest hard Creases or Corners.  This includes the regular
            //  corner and boundary cases that we don't want to isolate, but leaves a few others
            //  that do warrant isolation -- needing further inspection.
            //
            //  For now go with the boundary cases and don't isolate...
            selectFace = false;
        }
        if (selectFace) {
            selector.selectFace(face);
        }
    }
}

#ifdef _VTR_COMPUTE_MASK_WEIGHTS_ENABLED
void
TopologyRefiner::ComputeMaskWeights() {

    assert(_subdivType == Sdc::TYPE_CATMARK);

    for (int i = 0; i < _maxLevel; ++i) {
        _refinements[i].computeMaskWeights();
    }
}
#endif

} // end namespace Far

} // end namespace OPENSUBDIV_VERSION
} // end namespace OpenSubdiv

/****************************************************************************
* VCGLib                                                            o o     *
* Visual and Computer Graphics Library                            o     o   *
*                                                                _   O  _   *
* Copyright(C) 2004-2017                                           \/)\/    *
* Visual Computing Lab                                            /\/|      *
* ISTI - Italian National Research Council                           |      *
*                                                                    \      *
* All rights reserved.                                                      *
*                                                                           *
* This program is free software; you can redistribute it and/or modify      *
* it under the terms of the GNU General Public License as published by      *
* the Free Software Foundation; either version 2 of the License, or         *
* (at your option) any later version.                                       *
*                                                                           *
* This program is distributed in the hope that it will be useful,           *
* but WITHOUT ANY WARRANTY; without even the implied warranty of            *
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
* GNU General Public License (http://www.gnu.org/licenses/gpl.txt)          *
* for more details.                                                         *
*                                                                           *
****************************************************************************/
#ifndef _VCG_ISOTROPICREMESHING_H
#define _VCG_ISOTROPICREMESHING_H

#include<vcg/complex/algorithms/update/quality.h>
#include<vcg/complex/algorithms/update/curvature.h>
#include<vcg/complex/algorithms/update/normal.h>
#include<vcg/complex/algorithms/refine.h>
#include<vcg/complex/algorithms/stat.h>
#include<vcg/complex/algorithms/smooth.h>
#include<vcg/complex/algorithms/local_optimization/tri_edge_collapse.h>
#include<vcg/space/index/spatial_hashing.h>

#include <wrap/io_trimesh/export.h>

namespace vcg {
namespace tri {
template<class TRI_MESH_TYPE>
class IsotropicRemeshing
{
public:
	typedef TRI_MESH_TYPE MeshType;
	typedef typename MeshType::FaceType FaceType;
	typedef typename FaceType::VertexType VertexType;
	typedef typename FaceType::VertexPointer VertexPointer;
	typedef	typename VertexType::ScalarType ScalarType;
	typedef	typename VertexType::CoordType CoordType;
	typedef typename face::Pos<FaceType> PosType;
	typedef BasicVertexPair<VertexType> VertexPair;
	typedef EdgeCollapser<MeshType, VertexPair> Collapser;
	typedef GridStaticPtr<FaceType, ScalarType> StaticGrid;


	typedef struct Params {
		typedef struct Stat {
			int splitNum;
			int collapseNum;
			int flipNum;

			void Reset() {
				splitNum=0;
				collapseNum=0;
				flipNum=0;
			}
		} Stat;


		ScalarType minLength; // minimal admitted length: no edge should be shorter than this value (used when collapsing)
		ScalarType maxLength; // maximal admitted length: no edge should be longer than this value  (used when refining)
		ScalarType lengthThr;

		ScalarType minimalAdmittedArea;
		ScalarType maxSurfDist;

		ScalarType aspectRatioThr  = 0.05;                    //min aspect ratio: during relax bad triangles will be relaxed
		ScalarType foldAngleCosThr = cos(math::ToRad(140.));   //min angle to be considered folding: during relax folded triangles will be relaxed

		ScalarType creaseAngleRadThr = math::ToRad(10.0);
		ScalarType creaseAngleCosThr = cos(math::ToRad(10.0)); //min angle to be considered crease: two faces with normals diverging more than thr share a crease edge

		bool splitFlag    = true;
		bool swapFlag     = true;
		bool collapseFlag = true;
		bool smoothFlag=true;
		bool projectFlag=true;
		bool selectedOnly = false;

		bool userSelectedCreases = false;
		bool surfDistCheck = true;

		bool adapt=false;
		int iter=1;
		Stat stat;
		void SetTargetLen(const ScalarType len)
		{
			minLength=len*4./5.;
			maxLength=len*4./3.;
			lengthThr=len*4./3.;
			minimalAdmittedArea = (minLength * minLength)/1000.0;
		}
		void SetFeatureAngleDeg(const ScalarType angle)
		{
			creaseAngleRadThr =  math::ToRad(angle);
			creaseAngleCosThr = cos(creaseAngleRadThr);
		}

		StaticGrid grid;
		MeshType* m;
		MeshType* mProject;

	} Params;

	static void Do(MeshType &toRemesh, Params & params, vcg::CallBackPos * cb=0)
	{
		MeshType toProjectCopy;
		tri::UpdateBounding<MeshType>::Box(toRemesh);
		tri::UpdateNormal<MeshType>::PerVertexNormalizedPerFaceNormalized(toRemesh);

		tri::Append<MeshType,MeshType>::MeshCopy(toProjectCopy, toRemesh);

		Do(toRemesh,toProjectCopy,params,cb);
	}
	static void Do(MeshType &toRemesh, MeshType &toProject, Params & params, vcg::CallBackPos * cb=0)
	{
		assert(&toRemesh != &toProject);
		params.stat.Reset();

		tri::UpdateBounding<MeshType>::Box(toRemesh);

		{
			tri::UpdateBounding<MeshType>::Box(toProject);
			tri::UpdateNormal<MeshType>::PerFaceNormalized(toProject);
			params.m = &toRemesh;
			params.mProject = &toProject;
			params.grid.Set(toProject.face.begin(), toProject.face.end());
		}

		tri::UpdateTopology<MeshType>::FaceFace(toRemesh);
		tri::UpdateFlags<MeshType>::VertexBorderFromFaceAdj(toRemesh);
		tri::UpdateTopology<MeshType>::VertexFace(toRemesh);

		//		computeQuality(toRemesh);
		//		tri::UpdateQuality<MeshType>::VertexSaturate(toRemesh);

		tagCreaseEdges(toRemesh, params);

		for(int i=0; i < params.iter; ++i)
		{
			//			params.stat.Reset();

			if(cb) cb(100*i/params.iter, "Remeshing");

			if(params.splitFlag)
				SplitLongEdges(toRemesh, params);
#ifdef DEBUG_CREASE
			ForEachVertex(toRemesh, [] (VertexType & v) {
				v.C() = Color4b::Gray;
				v.Q() = 0;
			});

			ForEachFacePos(toRemesh, [&](PosType &p){
				if (p.F()->IsFaceEdgeS(p.E()))
				{
					p.V()->Q() += 1;
					p.VFlip()->Q() += 1;
				}
			});

			ForEachVertex(toRemesh, [] (VertexType & v) {
				if (v.Q() >= 4)
					v.C() = Color4b::Green;
				else if (v.Q() >= 2)
					v.C() = Color4b::Red;
			});
			std::string name = "creases" + std::to_string(i) + ".ply";
			vcg::tri::io::Exporter<MeshType>::Save(toRemesh, name.c_str(), vcg::tri::io::Mask::IOM_ALL);
#endif

			if(params.collapseFlag)
			{
				CollapseShortEdges(toRemesh, params);
				CollapseCrosses(toRemesh, params);

			}

			if(params.swapFlag)
				ImproveValence(toRemesh, params);

			if(params.smoothFlag)
				ImproveByLaplacian(toRemesh, params);

			if(params.projectFlag)
				ProjectToSurface(toRemesh, params);
		}
	}

private:
	/*
		TODO: Add better crease support: detect all creases at starting time, saving it on facedgesel flags
			  All operations must then preserve the faceedgesel flag accordingly:
				Refinement -> Check that refiner propagates faceedgesel [should be doing it]
				Collapse   -> Implement 1D edge collapse and better check on corners and creases
				Swap       -> Totally avoid swapping crease edges [ok]
				Smooth     -> Apply 1D smoothing to crease vertices + check on
								(http://www.cs.ubc.ca/labs/imager/tr/2009/eltopo/sisc2009.pdf)
	*/
	IsotropicRemeshing() {}
	// this returns the value of cos(a) where a is the angle between n0 and n1. (scalar prod is cos(a))
	static inline ScalarType fastAngle(Point3<ScalarType> n0, Point3<ScalarType> n1)
	{
		return math::Clamp(n0*n1,(ScalarType)-1.0,(ScalarType)1.0);
	}
	// compare the value of the scalar prod with the cos of the crease threshold
	static inline bool testCreaseEdge(PosType &p, ScalarType creaseCosineThr)
	{
		ScalarType angle = fastAngle(NormalizedTriangleNormal(*(p.F())), NormalizedTriangleNormal(*(p.FFlip())));
		return angle <= creaseCosineThr && angle >= -0.98;
		//        return (angle <= creaseCosineThr && angle >= -creaseCosineThr);
	}
	// this stores in minQ the value of the 10th percentile of the VertQuality distribution and in
	// maxQ the value of the 90th percentile.
	static inline void computeVQualityDistrMinMax(MeshType &m, ScalarType &minQ, ScalarType &maxQ)
	{
		Distribution<ScalarType> distr;
		tri::Stat<MeshType>::ComputePerVertexQualityDistribution(m,distr);

		maxQ = distr.Percentile(0.9f);
		minQ = distr.Percentile(0.1f);
	}

	//Computes PerVertexQuality as a function of the 'deviation' of the normals taken from
	//the faces incident to each vertex
	static void computeQuality(MeshType &m)
	{
		tri::RequirePerVertexQuality(m);
		tri::UpdateFlags<MeshType>::VertexClearV(m);

		for(auto vi=m.vert.begin(); vi!=m.vert.end(); ++vi)
			if(!(*vi).IsD())
			{
				vector<FaceType*> ff;
				face::VFExtendedStarVF(&*vi, 0, ff);

				ScalarType tot = 0.f;
				auto it = ff.begin();
				Point3<ScalarType> fNormal = NormalizedTriangleNormal(**it);
				++it;
				while(it != ff.end())
				{
					tot+= 1-math::Abs(fastAngle(fNormal, NormalizedTriangleNormal(**it)));
					++it;
				}
				vi->Q() = tot / (ScalarType)(std::max(1, ((int)ff.size()-1)));
				vi->SetV();
			}
	}

	/*
	 Computes the ideal valence for the vertex in pos p:
	 4 for border vertices
	 6 for internal vertices
	*/
	static inline int idealValence(PosType &p)
	{
		if(p.IsBorder()) return 4;
		return 6;
	}
	static inline int idealValence(VertexType &v)
	{
		if(v.IsB()) return 4;
		return 6;
	}
	static inline int idealValenceSlow(PosType &p)
	{
		std::vector<PosType> posVec;
		VFOrderedStarFF(p,posVec);
		float angleSumRad =0;
		for(PosType &ip : posVec)
		{
			angleSumRad += ip.AngleRad();
		}

		return (int)(std::ceil(angleSumRad / (M_PI/3.0f)));
	}

	static bool testHausdorff (MeshType & m, StaticGrid & grid, const std::vector<CoordType> & verts, const ScalarType maxD)
	{
		for (CoordType v : verts)
		{
			CoordType closest;
			ScalarType dist = 0;
			FaceType* fp = GetClosestFaceBase(m, grid, v, maxD, dist, closest);

			if (fp == NULL)
			{
				return false;
			}
		}
		return true;
	}

	static int tagCreaseEdges(MeshType &m, Params & params)
	{
		int count = 0;
		std::vector<char> creaseVerts(m.VN(), 0);

		vcg::tri::UpdateFlags<MeshType>::VertexClearV(m);
		std::queue<PosType> creaseQueue;
		ForEachFacePos(m, [&](PosType &p){
			if((p.FFlip() > p.F()) || p.IsBorder())
			{
				if (!params.userSelectedCreases && (testCreaseEdge(p, params.creaseAngleCosThr) || p.IsBorder()))
				{
					p.F()->SetFaceEdgeS(p.E());
					p.FlipF();
					p.F()->SetFaceEdgeS(p.E());
					creaseQueue.push(p);
				}
			}
		});

		//		//now all creases are checked...
		//		//prune false positive (too small) (count + scale?)

		//		while (!creaseQueue.empty())
		//		{
		//			PosType & p = creaseQueue.front();
		//			creaseQueue.pop();

		//			std::stack<PosType> chainQueue;
		//			std::vector<size_t> chainVerts;

		//			if (!p.V()->IsV())
		//			{
		//				chainQueue.push(p);
		//			}

		//			p.FlipV();
		//			p.NextEdgeS();

		//			if (!p.V()->IsV())
		//			{
		//				chainQueue.push(p);
		//			}

		//			while (!chainQueue.empty())
		//			{
		//				PosType p = chainQueue.top();
		//				chainQueue.pop();

		//				p.V()->SetV();
		//				chainVerts.push_back(vcg::tri::Index(m, p.V()));

		//				PosType pp = p;

		//				//circle around vert in search for new crease edges
		//				do {
		//					pp.NextF(); //jump adj face
		//					pp.FlipE(); // this edge is already ok => jump to next
		//					if (pp.IsEdgeS())
		//					{
		//						PosType nextPos = pp;
		//						nextPos.FlipV(); // go to next vert in the chain
		//						if (!nextPos.V()->IsV()) // if already visited...ignore
		//						{
		//							chainQueue.push(nextPos);
		//						}
		//					}
		//				}
		//				while (pp != p);

		//			}

		//			if (chainVerts.size() > 5)
		//			{
		//				for (auto vp : chainVerts)
		//				{
		//					creaseVerts[vp] = 1;
		//				}
		//			}
		//		}
		//		//store crease on V()

		//		//this aspect ratio check doesn't work on cadish meshes (long thin triangles spanning whole mesh)
		//		ForEachFace(m, [&] (FaceType & f) {
		//			if (vcg::QualityRadii(f.cP(0), f.cP(1), f.cP(2)) < params.aspectRatioThr)
		//			{
		//				if (creaseVerts[vcg::tri::Index(m, f.V(0))] == 0)
		//					f.V(0)->SetS();
		//				if (creaseVerts[vcg::tri::Index(m, f.V(1))] == 0)
		//					f.V(1)->SetS();
		//				if (creaseVerts[vcg::tri::Index(m, f.V(2))] == 0)
		//					f.V(2)->SetS();
		//			}
		//		});

		//		ForEachFace(m, [&] (FaceType & f) {
		//			for (int i = 0; i < 3; ++i)
		//			{
		//				if (f.FFp(i) > &f)
		//				{
		//					ScalarType angle = fastAngle(NormalizedTriangleNormal(f), NormalizedTriangleNormal(*(f.FFp(i))));
		//					if (angle <= params.foldAngleCosThr)
		//					{
		//						//						if (creaseVerts[vcg::tri::Index(m, f.V0(i))] == 0)
		//						f.V0(i)->SetS();
		//						//						if (creaseVerts[vcg::tri::Index(m, f.V1(i))] == 0)
		//						f.V1(i)->SetS();
		//						//						if (creaseVerts[vcg::tri::Index(m, f.V2(i))] == 0)
		//						f.V2(i)->SetS();
		//						//						if (creaseVerts[vcg::tri::Index(m, f.FFp(i)->V2(f.FFi(i)))] == 0)
		//						f.FFp(i)->V2(f.FFi(i))->SetS();
		//					}
		//				}
		//			}
		//		});

		return count;
	}


	/*
		Edge Swap Step:
		This method optimizes the valence of each vertex.
		oldDist is the sum of the absolute distance of each vertex from its ideal valence
		newDist is the sum of the absolute distance of each vertex from its ideal valence after
		the edge swap.
		If the swap decreases the total absolute distance, then it's applied, preserving the triangle
		quality.                        +1
			   v1                     v1
			  /  \                   /|\
			 /    \                 / | \
			/      \               /  |  \
		   /     _*p\           -1/   |   \ -1
		  v2--------v0 ========> v2   |   v0
		   \        /             \   |   /
			\      /               \  |  /
			 \    /                 \ | /
			  \  /                   \|/ +1
			   v3                     v3
			Before Swap             After Swap
	*/
	static bool testSwap(PosType p, ScalarType creaseAngleCosThr)
	{
		//if border or feature, do not swap
		if (/*p.IsBorder() || */p.IsEdgeS()) return false;

		int oldDist = 0, newDist = 0, idealV, actualV;

		PosType tp=p;

		VertexType *v0=tp.V();

		std::vector<VertexType*> incident;

		vcg::face::VVStarVF<FaceType>(tp.V(), incident);
		idealV  = idealValence(tp); actualV = incident.size();
		oldDist += abs(idealV - actualV); newDist += abs(idealV - (actualV - 1));

		tp.NextF();tp.FlipE();tp.FlipV();
		VertexType *v1=tp.V();
		vcg::face::VVStarVF<FaceType>(tp.V(), incident);
		idealV  = idealValence(tp); actualV = incident.size();
		oldDist += abs(idealV - actualV); newDist += abs(idealV - (actualV + 1));

		tp.FlipE();tp.FlipV();tp.FlipE();
		VertexType *v2=tp.V();
		vcg::face::VVStarVF<FaceType>(tp.V(), incident);
		idealV  = idealValence(tp); actualV = incident.size();
		oldDist += abs(idealV - actualV); newDist += abs(idealV - (actualV - 1));

		tp.NextF();tp.FlipE();tp.FlipV();
		VertexType *v3=tp.V();
		vcg::face::VVStarVF<FaceType>(tp.V(), incident);
		idealV  = idealValence(tp); actualV = incident.size();
		oldDist += abs(idealV - actualV); newDist += abs(idealV - (actualV + 1));

		ScalarType qOld = std::min(Quality(v0->P(),v2->P(),v3->P()),Quality(v0->P(),v1->P(),v2->P()));
		ScalarType qNew = std::min(Quality(v0->P(),v1->P(),v3->P()),Quality(v2->P(),v3->P(),v1->P()));

		return (newDist < oldDist && qNew >= qOld * 0.50f) ||
		        (newDist == oldDist && qNew > qOld * 1.f) || qNew > 1.5f * qOld;
	}

	static bool checkManifoldness(FaceType & f, int z)
	{
		PosType pos(&f, (z+2)%3, f.V2(z));
		PosType start = pos;

		do {
			pos.FlipE();
			if (!face::IsManifold(*pos.F(), pos.E()))
				break;
			pos.FlipF();
		} while (pos!=start);

		return pos == start;
	}

	// Edge swap step: edges are flipped in order to optimize valence and triangle quality across the mesh
	static void ImproveValence(MeshType &m, Params &params)
	{
		static ScalarType foldCheckRad = math::ToRad(5.);
		tri::UpdateTopology<MeshType>::FaceFace(m);
		tri::UpdateTopology<MeshType>::VertexFace(m);
		ForEachFace(m, [&] (FaceType & f) {
			if (face::IsManifold(f, 0) && face::IsManifold(f, 1) && face::IsManifold(f, 2))
				for (int i = 0; i < 3; ++i)
				{
					if (&f > f.cFFp(i))
					{
						PosType pi(&f, i);
						CoordType swapEdgeMidPoint = (f.cP2(i) + f.cFFp(i)->cP2(f.cFFi(i))) / 2.;
						std::vector<CoordType> toCheck(1, swapEdgeMidPoint);

						if(((!params.selectedOnly) || (f.IsS() && f.cFFp(i)->IsS())) &&
						   face::IsManifold(f, i) && checkManifoldness(f, i) &&
						   face::CheckFlipEdge(f, i) &&
						   testSwap(pi, params.creaseAngleCosThr) &&
						   (!params.surfDistCheck || testHausdorff(*params.mProject, params.grid, toCheck, params.maxSurfDist)) &&
						   face::CheckFlipEdgeNormal(f, i, vcg::math::ToRad(5.)))
						{
							//When doing the swap we need to preserve and update the crease info accordingly
							FaceType* g = f.cFFp(i);
							int w = f.FFi(i);

							bool creaseF = g->IsFaceEdgeS((w + 1) % 3);
							bool creaseG = f.IsFaceEdgeS((i + 1) % 3);

							face::FlipEdge(f, i);

							f.ClearFaceEdgeS((i + 1) % 3);
							g->ClearFaceEdgeS((w + 1) % 3);

							if (creaseF)
								f.SetFaceEdgeS(i);
							if (creaseG)
								g->SetFaceEdgeS(w);

							++params.stat.flipNum;
							break;
						}
					}
				}
		});
	}

	// The predicate that defines which edges should be split
	class EdgeSplitAdaptPred
	{
	public:
		int count = 0;
		ScalarType length, lengthThr, minQ, maxQ;
		bool operator()(PosType &ep)
		{
			ScalarType mult = math::ClampedLerp((ScalarType)0.5,(ScalarType)1.5, (((math::Abs(ep.V()->Q())+math::Abs(ep.VFlip()->Q()))/(ScalarType)2.0)/(maxQ-minQ)));
			ScalarType dist = Distance(ep.V()->P(), ep.VFlip()->P());
			if(dist > std::max(mult*length,lengthThr*2))
			{
				++count;
				return true;
			}
			else
				return false;
		}
	};

	class EdgeSplitLenPred
	{
	public:
		int count = 0;
		ScalarType squaredlengthThr;
		bool operator()(PosType &ep)
		{
			if(SquaredDistance(ep.V()->P(), ep.VFlip()->P()) > squaredlengthThr)
			{
				++count;
				return true;
			}
			else
				return false;
		}
	};

	//Split pass: This pass uses the tri::RefineE from the vcglib to implement
	//the refinement step, using EdgeSplitPred as a predicate to decide whether to split or not
	static void SplitLongEdges(MeshType &m, Params &params)
	{
		tri::UpdateTopology<MeshType>::FaceFace(m);
		tri::MidPoint<MeshType> midFunctor(&m);

		ScalarType minQ,maxQ;
		if(params.adapt){
			computeVQualityDistrMinMax(m, minQ, maxQ);
			EdgeSplitAdaptPred ep;
			ep.minQ      = minQ;
			ep.maxQ      = maxQ;
			ep.length    = params.maxLength;
			ep.lengthThr = params.lengthThr;
			tri::RefineE(m,midFunctor,ep);
			params.stat.splitNum+=ep.count;
		}
		else {
			EdgeSplitLenPred ep;
			ep.squaredlengthThr = params.maxLength*params.maxLength;
			tri::RefineMidpoint(m, ep, params.selectedOnly);
			params.stat.splitNum+=ep.count;
		}
	}

	static int VtoE(const int v0, const int v1)
	{
		static /*constexpr*/ int Vmat[3][3] = { -1,  0,  2,
		                                        0, -1,  1,
		                                        2,  1, -1};
		return Vmat[v0][v1];
	}


	static bool checkCanMoveOnCollapse(PosType p, std::vector<FaceType*> & faces, std::vector<int> & vIdxes, Params &params)
	{
		bool allIncidentFaceSelected = true;

		PosType pi = p;

		CoordType dEdgeVector = (p.V()->cP() - p.VFlip()->cP()).Normalize();

		for (size_t i = 0; i < faces.size(); ++i)
		{
			if (faces[i]->IsFaceEdgeS(VtoE(vIdxes[i], (vIdxes[i]+1)%3)))
			{
				CoordType movingEdgeVector0 = (faces[i]->cP1(vIdxes[i]) - faces[i]->cP(vIdxes[i])).Normalize();
				if (std::fabs(movingEdgeVector0 * dEdgeVector) < 1.f)
					return false;
			}
			if (faces[i]->IsFaceEdgeS(VtoE(vIdxes[i], (vIdxes[i]+2)%3)))
			{
				CoordType movingEdgeVector1 = (faces[i]->cP2(vIdxes[i]) - faces[i]->cP(vIdxes[i])).Normalize();
				if (std::fabs(movingEdgeVector1 * dEdgeVector) < 1.f)
					return false;
			}
			allIncidentFaceSelected &= faces[i]->IsS();
		}

		return params.selectedOnly ? allIncidentFaceSelected : true;
	}

	static bool checkFacesAfterCollapse (std::vector<FaceType*> & faces, PosType p, const Point3<ScalarType> &mp, Params &params, bool relaxed)
	{
		for (FaceType* f : faces)
		{
			if(!(*f).IsD() && f != p.F()) //i'm not a deleted face
			{
				PosType pi(f, p.V()); //same vertex

				VertexType *v0 = pi.V();
				VertexType *v1 = pi.F()->V1(pi.VInd());
				VertexType *v2 = pi.F()->V2(pi.VInd());

				if( v1 == p.VFlip() || v2 == p.VFlip()) //i'm the other deleted face
					continue;

				//check on new face quality
				{
					ScalarType newQ = Quality(mp,      v1->P(), v2->P());
					ScalarType oldQ = Quality(v0->P(), v1->P(), v2->P());

					if( newQ <= 0.5*oldQ  )
						return false;
				}

				// we prevent collapse that makes edges too long (except for cross)
				if(!relaxed)
					if((Distance(mp, v1->P()) > params.maxLength || Distance(mp, v2->P()) > params.maxLength))
						return false;

				Point3<ScalarType> oldN = NormalizedTriangleNormal(*(pi.F()));
				Point3<ScalarType> newN = Normal(mp, v1->P(), v2->P()).Normalize();

				float div = fastAngle(oldN, newN);
				if(div < 0.0 ) return false;

//				//				check on new face distance from original mesh
				if (params.surfDistCheck)
				{
					std::vector<CoordType> points(4);
					points[0] = (v1->cP() + v2->cP() + mp) / 3.;
					points[1] = (v1->cP() + mp) / 2.;
					points[2] = (v2->cP() + mp) / 2.;
					points[3] = mp;
					if (!testHausdorff(*(params.mProject), params.grid, points, params.maxSurfDist))
						return false;
				}
			}
		}
		return true;
	}


	//TODO: Refactor code and implement the correct set up of crease info when collapsing towards a crease edge
	static bool checkCollapseFacesAroundVert1(PosType &p, Point3<ScalarType> &mp, Params &params, bool relaxed)
	{
		PosType p0 = p, p1 = p;

		p1.FlipV();

		vector<int> vi0, vi1;
		vector<FaceType*> ff0, ff1;

		face::VFStarVF<FaceType>(p0.V(), ff0, vi0);
		face::VFStarVF<FaceType>(p1.V(), ff1, vi1);

		//check crease-moveability
		bool moveable0 = checkCanMoveOnCollapse(p0, ff0, vi0, params);
		bool moveable1 = checkCanMoveOnCollapse(p1, ff1, vi1, params);

		//if both moveable => go to midpoint
		// else collapse on movable one
		if (!moveable0 && !moveable1)
			return false;

		//casting int(true) is always 1 and int(false) = =0
		assert(int(true) == 1);
		assert(int(false) == 0);
		mp = (p0.V()->cP() * int(moveable1) + p1.V()->cP() * int(moveable0)) / (int(moveable0) + int(moveable1));

		if (!moveable0)
			p = p0;
		else
			p = p1;

		if (checkFacesAfterCollapse(ff0, p0, mp, params, relaxed))
			return checkFacesAfterCollapse(ff1, p1, mp, params, relaxed);

		return false;
	}

//	//Geometric check on feasibility of the collapse of the given pos
//	//The check fails if:
//	//  -new face has too bad quality.
//	//  -new face normal changes too much after collapse.
//	//  -new face has too long edges.
//	// TRY: if the vertex has valence 4 (cross vertex) we relax the check on length
//	//TODO: Refine the crease preservance check when collapsing along boundary (or in general maybe) WORK on this
//	static bool checkCollapseFacesAroundVert(PosType &p, Point3<ScalarType> &mp, Params & params, bool relaxed=false, bool crease=false)
//	{
//		ScalarType minimalAdmittedArea = (params.minLength * params.minLength)/10000.0;

//		vector<FaceType*> ff;
//		vector<int> vi;
//		face::VFStarVF<FaceType>(p.V(), ff, vi);

//		bool allIncidentFaceSelected = true;

//		for(FaceType *f: ff)
//			if(!(*f).IsD() && f != p.F()) //i'm not a deleted face
//			{
//				allIncidentFaceSelected &= f->IsS();

//				PosType pi(f, p.V()); //same vertex

//				VertexType *v0 = pi.V();
//				VertexType *v1 = pi.F()->V1(pi.VInd());
//				VertexType *v2 = pi.F()->V2(pi.VInd());

//				if( v1 == p.VFlip() || v2 == p.VFlip()) //i'm the other deleted face
//					continue;

//				//check on new face area
//				{
//					float area = DoubleArea(*(pi.F()))/2.f;

//					if (area < params.minimalAdmittedArea)
//						return false;
//				}



//				float area = DoubleArea(*(pi.F()))/2.f;

//				//quality and normal divergence checks
//				ScalarType newQ = Quality(mp,      v1->P(), v2->P());
//				ScalarType oldQ = Quality(v0->P(), v1->P(), v2->P());

//				if(area > minimalAdmittedArea) // for triangles not too small
//				{
//					if( newQ <= 0.5*oldQ  )
//						return false;

//					// we prevent collapse that makes edges too long (except for cross)
//					if(!relaxed)
//						if((Distance(mp, v1->P()) > params.maxLength || Distance(mp, v2->P()) > params.maxLength))
//							return false;

//					Point3<ScalarType> oldN = NormalizedTriangleNormal(*(pi.F()));
//					Point3<ScalarType> newN = Normal(mp, v1->P(), v2->P()).Normalize();
////					float div = fastAngle(oldN, newN);
////					if(crease && div < 0.98) return false;
////					{
////						std::vector<CoordType> points(3);
////						points[0] = (v1->cP() + v2->cP() + mp) / 3.;
////						points[1] = (v1->cP() + v0->cP()) / 2.;
////						points[2] = (v2->cP() + v0->cP()) / 2.;

////						if (!testHausdorff(*(params.mProject), params.grid, points, params.maxSurfDist))
////							return false;

////					}
//				}
//			}

//		if(params.selectedOnly) return allIncidentFaceSelected;
//		return true;
//	}

	static bool testCollapse1(PosType &p, Point3<ScalarType> &mp, ScalarType minQ, ScalarType maxQ, Params &params, bool relaxed = false)
	{
		ScalarType mult = (params.adapt) ? math::ClampedLerp((ScalarType)0.5,(ScalarType)1.5, (((math::Abs(p.V()->Q())+math::Abs(p.VFlip()->Q()))/(ScalarType)2.0)/(maxQ-minQ))) : (ScalarType)1;
		ScalarType dist = Distance(p.V()->P(), p.VFlip()->P());
		ScalarType thr = mult*params.minLength;
		ScalarType area = DoubleArea(*(p.F()))/2.f;
		if(relaxed || (dist < thr || area < params.minLength*params.minLength/100.f))//if to collapse
		{
			return checkCollapseFacesAroundVert1(p, mp, params, relaxed);
		}
		return false;
	}

	//This function is especially useful to enforce feature preservation during collapses
	//of boundary edges in planar or near planar section of the mesh
	static bool chooseBoundaryCollapse(PosType &p, VertexPair &pair)
	{
		Point3<ScalarType> collapseNV, collapsedNV0, collapsedNV1;
		collapseNV = (p.V()->P() - p.VFlip()->P()).normalized();

		vector<VertexType*> vv;
		face::VVStarVF<FaceType>(p.V(), vv);

		for(VertexType *v: vv)
			if(!(*v).IsD() && (*v).IsB() && v != p.VFlip()) //ignore non border
				collapsedNV0 = ((*v).P() - p.VFlip()->P()).normalized(); //edge vector after collapse

		face::VVStarVF<FaceType>(p.VFlip(), vv);

		for(VertexType *v: vv)
			if(!(*v).IsD() && (*v).IsB() && v != p.V()) //ignore non border
				collapsedNV1 = ((*v).P() - p.V()->P()).normalized(); //edge vector after collapse

		float cosine = cos(math::ToRad(1.5f));
		float angle0 = fabs(fastAngle(collapseNV, collapsedNV0));
		float angle1 = fabs(fastAngle(collapseNV, collapsedNV1));
		//if on both sides we deviate too much after collapse => don't collapse
		if(angle0 <= cosine && angle1 <= cosine)
			return false;
		//choose the best collapse (the more parallel one to the previous edge..)
		pair = (angle0 >= angle1) ? VertexPair(p.V(), p.VFlip()) : VertexPair(p.VFlip(), p.V());
		return true;
	}

	//The actual collapse step: foreach edge it is collapse iff TestCollapse returns true AND
	// the linkConditions are preserved
	static void CollapseShortEdges(MeshType &m, Params &params)
	{
		ScalarType minQ, maxQ;
		int candidates = 0;

		if(params.adapt)
			computeVQualityDistrMinMax(m, minQ, maxQ);

		tri::UpdateTopology<MeshType>::FaceFace(m);
		tri::UpdateTopology<MeshType>::VertexFace(m);
		tri::UpdateFlags<MeshType>::FaceBorderFromVF(m);
		tri::UpdateFlags<MeshType>::VertexBorderFromFaceBorder(m);
		tri::UpdateFlags<MeshType>::FaceClearS(m);

		for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
			if(!(*fi).IsD() && (params.selectedOnly == false || fi->IsS()))
			{
				for(auto i=0; i<3; ++i)
				{
					PosType pi(&*fi, i);
					++candidates;
					VertexPair  bp = VertexPair(pi.V(), pi.VFlip());
					Point3<ScalarType> mp = (pi.V()->P()+pi.VFlip()->P())/2.f;

					if(testCollapse1(pi, mp, minQ, maxQ, params) && Collapser::LinkConditions(bp))
					{
						//collapsing on pi.V()
						bp = VertexPair(pi.VFlip(), pi.V());

						Collapser::Do(m, bp, mp, true);
						++params.stat.collapseNum;
						break;
					}

				}
			}
		Allocator<MeshType>::CompactEveryVector(m);
	}


	//Here I just need to check the faces of the cross, since the other faces are not
	//affected by the collapse of the internal faces of the cross.
	static bool testCrossCollapse(PosType &p, std::vector<FaceType*> ff, std::vector<int> vi, Point3<ScalarType> &mp, Params &params)
	{
		if(!checkFacesAfterCollapse(ff, p, mp, params, true))
			return false;
		return true;
	}

	//Choose the best way to collapse a cross based on the (external) cross vertices valence
	//and resulting face quality
	//                                      +0                   -1
	//             v1                    v1                    v1
	//            /| \                   /|\                  / \
	//           / |  \                 / | \                /   \
	//          /  |   \               /  |  \              /     \
	//         / *p|    \           -1/   |   \ -1       +0/       \+0
	//       v0-------- v2 ========> v0   |   v2    OR    v0-------v2
	//        \    |    /             \   |   /            \       /
	//         \   |   /               \  |  /              \     /
	//          \  |  /                 \ | /                \   /
	//           \ | /                   \|/ +0               \ / -1
	//             v3                     v3                   v3
	static bool chooseBestCrossCollapse(PosType &p, VertexPair& bp, vector<FaceType*> &ff)
	{
		vector<VertexType*> vv0, vv1, vv2, vv3;
		VertexType *v0, *v1, *v2, *v3;

		v0 = p.F()->V1(p.VInd());
		v1 = p.F()->V2(p.VInd());


		bool crease[4] = {false, false, false, false};

		crease[0] = p.F()->IsFaceEdgeS(VtoE(p.VInd(), (p.VInd()+1)%3));
		crease[1] = p.F()->IsFaceEdgeS(VtoE(p.VInd(), (p.VInd()+2)%3));

		for(FaceType *f: ff)
			if(!(*f).IsD() && f != p.F())
			{
				PosType pi(f, p.V());
				VertexType *fv1 = pi.F()->V1(pi.VInd());
				VertexType *fv2 = pi.F()->V2(pi.VInd());

				if(fv1 == v0 || fv2 == v0)
				{
					if (fv1 == 0)
					{
						v3 = fv2;
						crease[3] = f->IsFaceEdgeS(VtoE(pi.VInd(), (pi.VInd()+2)%3));
					}
					else
					{
						v3 = fv1;
						crease[3] = f->IsFaceEdgeS(VtoE(pi.VInd(), (pi.VInd()+1)%3));
					}
//					v3 = (fv1 == v0) ? fv2 : fv1;
				}

				if(fv1 == v1 || fv2 == v1)
				{
					if (fv1 == v1)
					{
						v2 = fv2;
						crease[2] = f->IsFaceEdgeS(VtoE(pi.VInd(), (pi.VInd()+2)%3));
					}
					else
					{
						v2 = fv1;
						crease[2] = f->IsFaceEdgeS(VtoE(pi.VInd(), (pi.VInd()+1)%3));
					}
//					v2 = (fv1 == v1) ? fv2 : fv1;
				}
			}

		face::VVStarVF<FaceType>(v0, vv0);
		face::VVStarVF<FaceType>(v1, vv1);
		face::VVStarVF<FaceType>(v2, vv2);
		face::VVStarVF<FaceType>(v3, vv3);

		int nv0 = vv0.size(), nv1 = vv1.size();
		int nv2 = vv2.size(), nv3 = vv3.size();

		int delta1 = (idealValence(*v0) - nv0) + (idealValence(*v2) - nv2);
		int delta2 = (idealValence(*v1) - nv1) + (idealValence(*v3) - nv3);

		ScalarType Q1 = std::min(Quality(v0->P(), v1->P(), v3->P()), Quality(v1->P(), v2->P(), v3->P()));
		ScalarType Q2 = std::min(Quality(v0->P(), v1->P(), v2->P()), Quality(v2->P(), v3->P(), v0->P()));

		if (crease[0] || crease[1] || crease[2] || crease[3])
			return false;
//		if (crease[0] && crease[1] && crease[2] && crease[3])
//		{
//			return false;
//		}

//		if (crease[0] || crease[2])
//		{
//			bp = VertexPair(p.V(), v0);
//			return true;
//		}

//		if (crease[1] || crease[3])
//		{
//			bp = VertexPair(p.V(), v1);
//			return true;
//		}

		//no crease
		if(delta1 < delta2 && Q1 >= 0.6f*Q2)
		{
			bp = VertexPair(p.V(), v1);
			return true;
		}
		else
		{
			bp = VertexPair(p.V(), v0);
			return true;
		}
	}
	//Cross Collapse pass: This pass cleans the mesh from cross vertices, keeping in mind the link conditions
	//and feature preservations tests.
	static void CollapseCrosses(MeshType &m , Params &params)
	{
		tri::UpdateTopology<MeshType>::ClearFaceFace(m);
		tri::UpdateTopology<MeshType>::VertexFace(m);
		tri::UpdateFlags<MeshType>::VertexBorderFromNone(m);
		int count = 0;

		for(auto fi=m.face.begin(); fi!=m.face.end(); ++fi)
			if(!(*fi).IsD() && (params.selectedOnly == false || fi->IsS()))
			{
				for(auto i=0; i<3; ++i)
				{
					PosType pi(&*fi, i);
					if(!pi.V()->IsB())
					{
						vector<FaceType*> ff;
						vector<int> vi;
						face::VFStarVF<FaceType>(pi.V(), ff, vi);

						//if cross need to check what creases you have and decide where to collapse accordingly
						//if tricuspidis need whenever you have at least one crease => can't collapse anywhere
						if(ff.size() == 4 || ff.size() == 3)
						{
//							VertexPair bp;
							VertexPair  bp = VertexPair(pi.V(), pi.VFlip());
							Point3<ScalarType> mp = (pi.V()->P()+pi.VFlip()->P())/2.f;

//							if (ff.size() == 4)
//							{
//								//avoid collapsing if creases don't allow to
////								continue;
//								if (!chooseBestCrossCollapse(pi, bp, ff))
//									continue;
//							}
//							else //tricuspidis
//							{
//								bool collapse = true;
//								for (int i = 0; i < ff.size(); ++i)
//								{
//									PosType pp(ff[i], pi.V());

//									if (pp.IsFaceS())
//										collapse = false;
//									pp.FlipE();
//									if (pp.IsFaceS())
//										collapse = false;
//								}
//								if (!collapse)
//									continue;
//								else bp =  VertexPair(pi.V(), pi.VFlip());
//							}

////							VertexPair bp  = (ff.size() == 4) ? chooseBestCrossCollapse(pi, ff) : VertexPair(pi.V(), pi.VFlip());
//							Point3<ScalarType> mp = bp.V(1)->P();

//							//todo: think about if you should try doing the other collapse if test or link fails for this one
//							if(testCrossCollapse(pi, ff, vi, mp, params) && Collapser::LinkConditions(bp))
							if(testCollapse1(pi, mp, 0, 0, params) && Collapser::LinkConditions(bp))
							{
								Collapser::Do(m, bp, mp, true);
								++count;
								break;
							}
						}
					}
				}
			}
		Allocator<MeshType>::CompactEveryVector(m);
	}

	// This function sets the selection bit on vertices that lie on creases
	static int selectVertexFromCrease(MeshType &m, ScalarType creaseThr)
	{
		int count = 0;
		ForEachFacePos(m, [&](PosType &p){
			if((p.FFlip() > p.F()) && p.IsEdgeS()/*testCreaseEdge(p, creaseThr)*/)
			{
				p.V()->SetS();
				p.VFlip()->SetS();
				++count;
			}
		});
		return count;
	}

	static int selectVertexFromFold(MeshType &m, Params & params)
	{
		std::vector<char> creaseVerts(m.VN(), 0);
		ForEachFacePos(m, [&] (PosType & p) {
			if (p.IsEdgeS())
			{
				creaseVerts[vcg::tri::Index(m, p.V())] = 1;
				creaseVerts[vcg::tri::Index(m, p.VFlip())] = 1;
			}
		});


		//this aspect ratio check doesn't work on cadish meshes (long thin triangles spanning whole mesh)
		ForEachFace(m, [&] (FaceType & f) {
			if (vcg::QualityRadii(f.cP(0), f.cP(1), f.cP(2)) < params.aspectRatioThr)
			{
				if (creaseVerts[vcg::tri::Index(m, f.V(0))] == 0)
					f.V(0)->SetS();
				if (creaseVerts[vcg::tri::Index(m, f.V(1))] == 0)
					f.V(1)->SetS();
				if (creaseVerts[vcg::tri::Index(m, f.V(2))] == 0)
					f.V(2)->SetS();
			}
		});


		ForEachFace(m, [&] (FaceType & f) {
			for (int i = 0; i < 3; ++i)
			{
				if (f.FFp(i) > &f)
				{
					ScalarType angle = fastAngle(NormalizedTriangleNormal(f), NormalizedTriangleNormal(*(f.FFp(i))));
					if (angle <= params.foldAngleCosThr)
					{
						if (creaseVerts[vcg::tri::Index(m, f.V0(i))] == 0)
							f.V0(i)->SetS();
						if (creaseVerts[vcg::tri::Index(m, f.V1(i))] == 0)
							f.V1(i)->SetS();
						if (creaseVerts[vcg::tri::Index(m, f.V2(i))] == 0)
							f.V2(i)->SetS();
						if (creaseVerts[vcg::tri::Index(m, f.FFp(i)->V2(f.FFi(i)))] == 0)
							f.FFp(i)->V2(f.FFi(i))->SetS();
					}
				}
			}
		});

		return 0;
	}

	static void FoldRelax(MeshType &m, Params params, const int step)
	{
		typename vcg::tri::Smooth<MeshType>::LaplacianInfo lpz(CoordType(0, 0, 0), 0);
		SimpleTempData<typename MeshType::VertContainer, typename vcg::tri::Smooth<MeshType>::LaplacianInfo> TD(m.vert, lpz);
		for (int i = 0; i < step; ++i)
		{
			TD.Init(lpz);
			vcg::tri::Smooth<MeshType>::AccumulateLaplacianInfo(m, TD, true);

			for (auto fi = m.face.begin(); fi != m.face.end(); ++fi)
			{
				std::vector<CoordType> newPos(4);
				bool moving = false;

				for (int j = 0; j < 3; ++j)
				{
					newPos[j] = fi->cP(j);
					if (!fi->V(j)->IsD() && TD[fi->V(j)].cnt > 0)
					{
						if (fi->V(j)->IsS())
						{
							newPos[j] = (fi->V(j)->P() + TD[fi->V(j)].sum) / (TD[fi->V(j)].cnt + 1);
							moving = true;
						}
					}
				}

				if (moving)
				{
					newPos[3] = (newPos[0] + newPos[1] + newPos[2]) / 3.;
					if (!params.surfDistCheck || testHausdorff(*params.mProject, params.grid, newPos, params.maxSurfDist))
					{
						for (int j = 0; j < 3; ++j)
							fi->V(j)->P() = newPos[j];
					}
				}
			}
		}
	}
	//	static int
	/**
	  * Simple Laplacian Smoothing step
	  * Border and crease vertices are kept fixed.
	  * If there are selected faces and the param.onlySelected is true we compute
	  * the set of internal vertices to the selection and we combine it in and with
	  * the vertexes not on border or creases
	*/
	static void ImproveByLaplacian(MeshType &m, Params params)
	{
		SelectionStack<MeshType> ss(m);

		if(params.selectedOnly) {
			ss.push();
			tri::UpdateSelection<MeshType>::VertexFromFaceStrict(m);
			ss.push();
		}
		tri::UpdateTopology<MeshType>::FaceFace(m);
		tri::UpdateFlags<MeshType>::VertexBorderFromFaceAdj(m);
		tri::UpdateSelection<MeshType>::VertexFromBorderFlag(m);
		selectVertexFromCrease(m, params.creaseAngleCosThr);
		tri::UpdateSelection<MeshType>::VertexInvert(m);
		if(params.selectedOnly) {
			ss.popAnd();
		}
		tri::Smooth<MeshType>::VertexCoordPlanarLaplacian(m, 1, math::ToRad(1.0), true);

		tri::UpdateSelection<MeshType>::VertexClear(m);

		selectVertexFromFold(m, params);
		FoldRelax(m, params, 3);

		tri::UpdateSelection<MeshType>::VertexClear(m);

		if(params.selectedOnly) {
			ss.pop();
		}
	}
	/*
		Reprojection step, this method reprojects each vertex on the original surface
		sampling the nearest Point3 onto it using a uniform grid StaticGrid t
	*/
	static void ProjectToSurface(MeshType &m, Params & params)
	{
		for(auto vi=m.vert.begin();vi!=m.vert.end();++vi)
			if(!(*vi).IsD())
			{
				Point3<ScalarType> newP, normP, barP;
				ScalarType maxDist = params.maxSurfDist * 1.5f, minDist = 0.f;
				FaceType* fp = GetClosestFaceBase(*params.mProject, params.grid, vi->cP(), maxDist, minDist, newP, normP, barP);

				if (fp != NULL)
				{
					vi->P() = newP;
				}
			}
	}
};
} // end namespace tri
} // end namespace vcg
#endif

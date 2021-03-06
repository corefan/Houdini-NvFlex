

#include "SIM_NvFlexSolver.h"

#include "SIM_NvFlexData.h" //for static library

#include <SIM/SIM_ObjectArray.h>
#include <SIM/SIM_Object.h>
#include <SIM/SIM_GeometryCopy.h>
#include <SIM/SIM_ForceGravity.h>
#include <GU/GU_Detail.h>
#include <PRM/PRM_Template.h>
#include <PRM/PRM_Default.h>
#include <PRM/PRM_Range.h>

#include <GA/GA_PageIterator.h>
#include <GA/GA_PageHandle.h>
#include <GA/GA_SplittableRange.h>

#include <algorithm>

#include "NvFlexHTriangleMesh.h"




SIM_NvFlexSolver::SIM_Result SIM_NvFlexSolver::solveObjectsSubclass(SIM_Engine & engine, SIM_ObjectArray & objs, SIM_ObjectArray & newobjs, SIM_ObjectArray & feedbackobjs, const SIM_Time & timestep)
{
	NvFlexHContextAutoGetter contextAutoGetAndRelease();

	for (exint obji = 0; obji < objs.entries(); ++obji) {
		SIM_Object* obj = objs(obji);

		SIM_NvFlexData* nvdata = SIM_DATA_GET(*obj, "NvFlexData", SIM_NvFlexData);
		if (nvdata == NULL)continue;
		if (!nvdata->isNvValid()) {
			addError(obj, SIM_BADSUBDATA, "NvFlexData is in invalid state (maybe insufficient GPU resources). try resetting the simulation.", UT_ERROR_WARNING);
			continue;
		}
		std::shared_ptr<SIM_NvFlexData::NvFlexContainerWrapper> consolv = nvdata->nvdata;


		// Getting old geometry and shoving it into NvFlex buffers
		const SIM_Geometry *geo=SIM_DATA_GETCONST(*obj, "Geometry", SIM_Geometry);
		if (geo != NULL) {
			GU_DetailHandleAutoReadLock lock(geo->getGeometry());
			if (lock.isValid()) {
				const GU_Detail *gdp = lock.getGdp();
				int64 ndid = gdp->getP()->getDataId();
				std::cout << "id = " << ndid << std::endl;
				if (ndid != nvdata->_lastGdpPId) {
					std::cout << "found geo, new id !! old id "<< nvdata->_lastGdpPId << std::endl;

					GA_ROHandleV3 phnd(gdp->getP());
					GA_ROHandleV3 vhnd(gdp->findPointAttribute("v"));
					GA_ROHandleI ihnd(gdp->findPointAttribute("iid"));
					GA_ROHandleI phshnd(gdp->findPointAttribute("phs"));
					GA_ROHandleF mhnd(gdp->findPointAttribute("imass"));
					GA_ROHandleV3 rhnd(gdp->findPointAttribute("restP"));
					const bool hasRest = rhnd.isValid();

					int* indices = nvdata->_indices.get();
					int nactives = NvFlexExtGetActiveList(consolv->container(), indices);

					if (phnd.isValid() && vhnd.isValid() && ihnd.isValid() && phshnd.isValid() && mhnd.isValid()) {
						NvFlexExtParticleData pdat = NvFlexExtMapParticleData(consolv->container());

						GA_Size ngdpoints = gdp->getNumPoints();
						bool reget = false;
						if (nactives < ngdpoints) {
							int nptscount=NvFlexExtAllocParticles(consolv->container(), ngdpoints - nactives, indices); //whoa! carefull with that! your luck the mapped buffer is not reallocated during this operation!
							/*for (int npi = 0; npi < nptscount; ++npi) {
								pdat.phases[indices[npi]] = eNvFlexPhaseSelfCollide | eNvFlexPhaseFluid;
							}*/
							reget = true;
						}
						else if (nactives > ngdpoints) {
							NvFlexExtFreeParticles(consolv->container(), nactives - ngdpoints, indices);
							reget = true;
						}
						if (reget) nactives = NvFlexExtGetActiveList(consolv->container(), indices);

						GA_Offset bst, bed;
						bool stoploop = false;
						for (GA_Iterator it(gdp->getPointRange()); it.blockAdvance(bst, bed);) { //TODO: make it threaded after debugged
							for (GA_Offset off = bst; off < bed; ++off) {
								UT_Vector3F p = phnd.get(off);
								UT_Vector3F v = vhnd.get(off);

								GA_Index idx = gdp->pointIndex(off);
								if (idx >= nactives) {
									stoploop = true;
									break;
								}
								int iid = indices[idx];
								int iid4 = iid * 4;
								int iid3 = iid * 3;
								pdat.particles[iid4 + 0] = p.x();
								pdat.particles[iid4 + 1] = p.y();
								pdat.particles[iid4 + 2] = p.z();
								pdat.particles[iid4 + 3] = mhnd.get(off);
								if (hasRest) {
									UT_Vector3F rst = rhnd.get(off);
									pdat.restParticles[iid4 + 0] = rst.x();
									pdat.restParticles[iid4 + 1] = rst.y();
									pdat.restParticles[iid4 + 2] = rst.z();
									pdat.restParticles[iid4 + 3] = 1.0f; //cannot find in manual what it expects here
								}

								pdat.velocities[iid3 + 0] = v.x();
								pdat.velocities[iid3 + 1] = v.y();
								pdat.velocities[iid3 + 2] = v.z();

								pdat.phases[iid] = phshnd.get(off);
							}
							if (stoploop)break;
						}
						
						NvFlexExtUnmapParticleData(consolv->container());

						//Push NvFlex data to GPU. since it's async - we need to do it as far from the solver tick as possible to use this time to do CPU work
						NvFlexExtPushToDevice(consolv->container()); //This pushes all from particle data returned by map. so collisions, springs and triangles we can push separately.
						//Also note that as long as we don't call anything with nvFlexExtAssets - we are free to rebind springs manually.

						GA_Size nprims = gdp->getNumPrimitives();
						if(nprims>0){//Create and Push SPRINGS and TRIANGLES
							GA_ROHandleF rlhnd(gdp->findPrimitiveAttribute("restlength"));
							GA_ROHandleF sthnd(gdp->findPrimitiveAttribute("strength"));
							GA_ROHandleV3 nphnd(gdp->findPointAttribute("N"));
							GA_ROHandleV3 nvhnd(gdp->findVertexAttribute("N"));
							GA_ROHandleV3 nrhnd(gdp->findPrimitiveAttribute("N"));
							const short triNormalType = nrhnd.isValid() ? 3 : (nvhnd.isValid() ? 2 : (nphnd.isValid() ? 1 : 0));


							if (rlhnd.isValid() && sthnd.isValid()) {
								//This would be super not cool and not optimal to do. But in NvFlexVector capacity is never lowered, so it's safe and fast to resize down later
								// as a downside - we always will have that extra memory allocated untill solver is resetted
								consolv->resizeSpringData(nprims);
								consolv->resizeTriangleData(nprims); //TODO: count properly, dont waste memory like this!

								auto sprdat = consolv->mapSpringData();
								auto tridat = consolv->mapTriangleData();
								GA_Size springcount = 0; //TODO: replace with SYS_AtomicCounter in threaded implementation
								GA_Size trianglecount = 0;
								for (GA_Iterator it(gdp->getPrimitiveRange()); !it.atEnd(); ++it) {
									GA_Offset off = *it;
									GA_Size vtxcount = gdp->getPrimitiveVertexCount(off);
									if (vtxcount == 2) {
										GA_OffsetListRef vtxs = gdp->getPrimitiveVertexList(off);
										GA_Offset vt0 = vtxs(0);
										GA_Offset vt1 = vtxs(1);

										//TODO: check that if we hit pts limit - we dont write geo indices above the limit!!
										//at this point indices should still be valid
										sprdat.springIds[springcount * 2 + 0] = indices[gdp->pointIndex(gdp->vertexPoint(vt0))];
										sprdat.springIds[springcount * 2 + 1] = indices[gdp->pointIndex(gdp->vertexPoint(vt1))];
										sprdat.springRls[springcount] = rlhnd.get(off);
										sprdat.springSts[springcount] = sthnd.get(off);

										++springcount;
									}
									else if (vtxcount == 3) {
										GA_OffsetListRef vtxs = gdp->getPrimitiveVertexList(off);
										GA_Offset vt0 = vtxs(0);
										GA_Offset vt1 = vtxs(1);
										GA_Offset vt2 = vtxs(2);

										GA_Offset pt0 = gdp->vertexPoint(vt0);
										GA_Offset pt1 = gdp->vertexPoint(vt1);
										GA_Offset pt2 = gdp->vertexPoint(vt2);

										GA_Size tricnt3 = trianglecount * 3;
										tridat.triangleIds[tricnt3 + 0] = indices[gdp->pointIndex(pt0)];
										tridat.triangleIds[tricnt3 + 1] = indices[gdp->pointIndex(pt1)];
										tridat.triangleIds[tricnt3 + 2] = indices[gdp->pointIndex(pt2)];

										if (triNormalType > 0) {
											UT_Vector3F n;
											if (triNormalType == 1) {
												n = nphnd.get(pt0);
												n += nphnd.get(pt1);
												n += nphnd.get(pt2);
												n.normalize();
											}
											else if (triNormalType == 2) {
												n = nvhnd.get(vt0);
												n += nvhnd.get(vt1);
												n += nvhnd.get(vt2);
												n.normalize();
											}
											else if (triNormalType == 3) {
												n = nrhnd.get(off);
											}
											tridat.triangleNms[tricnt3 + 0] = n.x();
											tridat.triangleNms[tricnt3 + 1] = n.y();
											tridat.triangleNms[tricnt3 + 2] = n.z();
										}

										++trianglecount;
									}
								}
								consolv->unmapSpringData();
								consolv->unmapTriangleData();
								//TODO: check that springcount is in int bounds and clamp it if needed!!
								consolv->resizeSpringData((int)springcount);
								consolv->resizeTriangleData((int)trianglecount);


							}
							consolv->pushSpringsToDevice();//Note that we should do this only if change occured in springs. for now we do not detect those changes, so we push always.
							consolv->pushTrianglesToDevice(triNormalType > 0);
						}//END SPRINGS AND TRIANGLES

					}

				}

			}
		}

		
		
		
		// Updating collision Geometry.
		// TODO: kill/deactivate meshes that are no longer in relationships
		{
			NvFlexHCollisionData* colldata = consolv->collisionData();
			colldata->mapall();
			/*
			colldata->addSphere("test");
			colldata->getSphere("test").collgeo->radius = 1.0f;
			colldata->getSphere("test").position->y = 1.0f;
			colldata->getSphere("test").prevposition->y = 1.0f;
			*/

			//find collision relationships and build collisions
			SIM_ConstObjectArray affs;
			obj->getConstAffectors(affs, "SIM_RelationshipCollide");
			for (exint afi = 0; afi < affs.entries(); ++afi) {
				const SIM_Object* aff = affs(afi);
				if (aff == obj)continue;
				std::cout << aff->getName() << " : " << aff->getObjectId() << std::endl;
				const SIM_Geometry*affgeo = SIM_DATA_GETCONST(*aff, SIM_GEOMETRY_DATANAME, SIM_Geometry);
				if (affgeo == NULL)continue;

				GU_DetailHandleAutoReadLock hlk(affgeo->getGeometry());
				const GU_Detail *gdp = hlk.getGdp();
				int64 pDataId=gdp->getP()->getDataId();

				std::string objidname = std::to_string(aff->getObjectId());

				if(pDataId != colldata->getStoredHash(objidname)){
					std::cout << "updating mesh " << objidname << std::endl;
					colldata->setStoredHash(objidname, pDataId);
					colldata->addTriangleMesh(objidname);
					NvfTrimeshGeo trigeo=colldata->getTriangleMesh(objidname);

					NvFlexHTriangleMeshAutoMapper tmeshlock(trigeo.collgeo);


					GA_Offset off;
					tmeshlock.setVertexCount(gdp->getNumPoints());
					Vec3* trigeop = tmeshlock.vertices();
					float* trigeolw = tmeshlock.lower();
					float* trigeoup = tmeshlock.upper();
					trigeoup[0] = trigeoup[1] = trigeoup[2] = -FLT_MAX;
					trigeolw[0] = trigeolw[1] = trigeolw [2] = FLT_MAX;
					GA_FOR_ALL_PTOFF(gdp, off) {
						UT_Vector3 p=gdp->getPos3(off);
						Vec3* currtgp = trigeop + gdp->pointIndex(off);
						currtgp->x = p.x();
						currtgp->y = p.y();
						currtgp->z = p.z();
						trigeolw[0] = std::min(p.x(), trigeolw[0]);
						trigeolw[1] = std::min(p.y(), trigeolw[1]);
						trigeolw[2] = std::min(p.z(), trigeolw[2]);
						trigeoup[0] = std::max(p.x(), trigeoup[0]);
						trigeoup[1] = std::max(p.y(), trigeoup[1]);
						trigeoup[2] = std::max(p.z(), trigeoup[2]);
					}

					//now we need to calculate triangles
					GA_Size tricount = 0;
					for (GA_Iterator it(gdp->getPrimitiveRange()); !it.atEnd(); ++it) {
						tricount += std::max(gdp->getPrimitiveVertexCount(*it) - 2, GA_Size(0));
					}
					tmeshlock.setTrianglesCount(tricount);
					//now set triangles!
					size_t i = 0;
					int* trigeot = tmeshlock.triangles();
					for (GA_Iterator it(gdp->getPrimitiveRange()); !it.atEnd(); ++it) {
						GA_OffsetListRef pvlr=gdp->getPrimitiveVertexList(*it);
						GA_Index sttidx = -1;
						GA_Index prvidx = -1;
						for (int vi = 0; vi < pvlr.entries(); ++vi) {
							GA_Index idx = gdp->pointIndex(gdp->vertexPoint(pvlr(vi)));//gdp->vertexIndex(pvlr(vi));//
							if (vi == 0)sttidx = idx;
							else if (vi > 1) {
								//invert order cuz houdini goes clockwise
								trigeot[i++] = sttidx;
								trigeot[i++] = idx;
								trigeot[i++] = prvidx;
							}
							prvidx = idx;
						}
					}

				}
			}

			colldata->unmapall();
			colldata->setCollisionData(consolv->solver());
		}


		nvparams.numIterations = getIterations();
		int substeps = getSubsteps();
		updateSolverParams();
		//Find and apply gravity
		{
			SIM_ConstDataArray gravities;
			obj->filterConstSubData(gravities, 0, SIM_DataFilterByType("SIM_ForceGravity"), SIM_FORCES_DATANAME, SIM_DataFilterNone());
			for (exint i = 0; i < gravities.entries(); ++i) {
				const SIM_ForceGravity* force = SIM_DATA_CASTCONST(gravities(i), SIM_ForceGravity);
				if (force == NULL)continue;
				UT_Vector3 outForce, outTorque;
				force->getForce(*obj, UT_Vector3(), UT_Vector3(), UT_Vector3(), 1.0f, outForce,outTorque);

				nvparams.gravity[0] += outForce.x();
				nvparams.gravity[1] += outForce.y();
				nvparams.gravity[2] += outForce.z();
			}
		}
		NvFlexSetParams(consolv->solver(), &nvparams);

		NvFlexExtTickContainer(consolv->container(), timestep, substeps, false);

		NvFlexExtPullFromDevice(consolv->container());


		SIM_GeometryCopy *newgeo=SIM_DATA_CREATE(*obj, "Geometry", SIM_GeometryCopy, SIM_DATA_RETURN_EXISTING | SIM_DATA_ADOPT_EXISTING_ON_DELETE);
		if (newgeo == NULL)continue;//TODO: show error;
		GU_DetailHandleAutoWriteLock lock(newgeo->getOwnGeometry());
		if (lock.isValid()) {
			GU_Detail *dgp = lock.getGdp();

			int* iindex = nvdata->_indices.get(); //TODO: indices dont change - if we got them before solve - keep them!
			int nactives = NvFlexExtGetActiveList(consolv->container(), iindex); //HERE I REEEEALLY HOPE nooe accesses it right now
			
			const bool recreateGeo = nactives != dgp->getNumPoints(); //This basically should never happen with current workflow

			if(recreateGeo)dgp->stashAll();

			//GA_RWAttributeRef vatt = dgp->findPointAttribute("v");//
			GA_RWAttributeRef vatt = dgp->findFloatTuple(GA_ATTRIB_POINT, "v", 3, 3);
			if (!vatt.isValid()) {
				vatt = dgp->addFloatTuple(GA_ATTRIB_POINT, "v", 3, GA_Defaults(0));
				vatt.setTypeInfo(GA_TYPE_VECTOR);
			}
			//GA_RWAttributeRef iidatt = dgp->findPointAttribute("iid");
			GA_RWAttributeRef iidatt = dgp->findIntTuple(GA_ATTRIB_POINT, "iid", 1, 1);
			if (!iidatt.isValid()) {
				iidatt = dgp->addIntTuple(GA_ATTRIB_POINT, "iid", 1, GA_Defaults(-1));
			}
			//GA_RWAttributeRef phsatt = dgp->findPointAttribute("phs");
			GA_RWAttributeRef phsatt = dgp->findIntTuple(GA_ATTRIB_POINT, "phs", 1, 1);
			if (!phsatt.isValid()) {
				phsatt = dgp->addIntTuple(GA_ATTRIB_POINT, "phs", 1, GA_Defaults(0));
			}
			GA_RWHandleV3 vhd(vatt);
			GA_RWHandleI iidhd(iidatt);
			GA_RWHandleI phshd(phsatt);

			
			NvFlexExtParticleData pdat = NvFlexExtMapParticleData(consolv->container());	//mapping
			
			// get indices and go through active indices!
			if(recreateGeo)GA_Offset off = dgp->appendPointBlock(nactives);

			GA_Offset ostt, oend;
			for (GA_Iterator oit(dgp->getPointRange()); oit.blockAdvance(ostt, oend);) { //TODO: make it threaded after debugged
				for (GA_Offset curroff = ostt; curroff < oend; ++curroff) {
					UT_Vector3 pp;
					int ii = iindex[dgp->pointIndex(curroff)];
					pp.assign(pdat.particles[ii * 4 + 0], pdat.particles[ii * 4 + 1], pdat.particles[ii * 4 + 2]);
					dgp->setPos3(curroff, pp);
					pp.assign(pdat.velocities[ii * 3 + 0], pdat.velocities[ii * 3 + 1], pdat.velocities[ii * 3 + 2]);
					vhd.set(curroff, pp);
					iidhd.set(curroff, ii);
					phshd.set(curroff, pdat.phases[ii]);
				}
			}
			NvFlexExtUnmapParticleData(consolv->container());//unmapping

			if(recreateGeo)dgp->destroyStashed();
			dgp->getAttributes().bumpAllDataIds(GA_ATTRIB_POINT);
			nvdata->_lastGdpPId = dgp->getP()->getDataId(); //TODO: shit, we cannot save it on solver! save it on data!
		}

		
	}

	return SIM_SOLVER_SUCCESS;
}

void SIM_NvFlexSolver::initializeSubclass()
{
	SIM_Solver::initializeSubclass();
	//if(SIM_NvFlexData::nvFlexLibrary == NULL)SIM_NvFlexData::nvFlexLibrary=NvFlexInit();
	
	//if(!nvparams)nvparams.reset(new NvFlexParams); //we don't need to reset it every every time i guess
	
	//NvFlexGetParams(nvsolver->solver(), nvparams.get());
	updateSolverParams();

	//NvFlexSetParams(nvsolver->solver(), nvparams.get());
}

void SIM_NvFlexSolver::updateSolverParams() {
	nvparams.radius = getRadius();

	nvparams.gravity[0] = 0;
	nvparams.gravity[1] = 0;
	nvparams.gravity[2] = 0;
	nvparams.fluidRestDistance = nvparams.radius * getFluidRestDistanceMult();
	nvparams.solidRestDistance = nvparams.fluidRestDistance;
	nvparams.numIterations = getIterations();
	nvparams.maxSpeed = getMaxSpeed(); //FLT_MAX;
	nvparams.maxAcceleration = getMaxAcceleration(); //1000.0f;
	nvparams.fluid = true;

	nvparams.viscosity = getViscosity();
	nvparams.dynamicFriction = getDynamicFriction();
	nvparams.staticFriction = getStaticFriction();
	nvparams.particleFriction = getParticleFriction();//0.0f; // scale friction between particles by default
	nvparams.freeSurfaceDrag = 0.0f;
	nvparams.drag = getDrag();// 0.0f;
	nvparams.lift = getLift();// 0.0f;

	nvparams.numPlanes = getPlanesCount();
	(Vec4&)nvparams.planes[0] = Vec4(0.0f, 1.0f, 0.0f, 0.0f);
	(Vec4&)nvparams.planes[1] = Vec4(0.0f, 0.0f, 1.0f, 4);
	(Vec4&)nvparams.planes[2] = Vec4(1.0f, 0.0f, 0.0f, 2);
	(Vec4&)nvparams.planes[3] = Vec4(-1.0f, 0.0f, 0.0f, 2);
	(Vec4&)nvparams.planes[4] = Vec4(0.0f, 0.0f, -1.0f, 4);
	//(Vec4&)nvparams->planes[5] = Vec4(0.0f, -1.0f, 0.0f, g_sceneUpper.y);

	nvparams.anisotropyScale = 0.0f;
	nvparams.anisotropyMin = 0.1f;
	nvparams.anisotropyMax = 2.0f;
	nvparams.smoothing = 0.0f;

	nvparams.shapeCollisionMargin = getShapeCollisionMargin();
	nvparams.particleCollisionMargin = getParticleCollisionMargin();
	nvparams.collisionDistance = getCollisionDistance();

	nvparams.relaxationMode = eNvFlexRelaxationLocal;
	nvparams.relaxationFactor = getRelaxationFactor();// 1.0f;
	nvparams.solidPressure = getSolidPressure();// 0.1f;
	nvparams.adhesion = getAdhesion();
	nvparams.cohesion = getCohesion();
	nvparams.surfaceTension = getSurfaceTension();
	nvparams.vorticityConfinement = getVorticityConfinement();// 0.0f;
	nvparams.buoyancy = getBuoyancy();// 1.0f;
}

void SIM_NvFlexSolver::makeEqualSubclass(const SIM_Data * source)
{
	SIM_Solver::makeEqualSubclass(source);
	const SIM_NvFlexSolver *src = SIM_DATA_CASTCONST(source, SIM_NvFlexSolver);
	if (src == NULL) {
		return;
	}

	nvparams = src->nvparams;
}





const SIM_DopDescription* SIM_NvFlexSolver::getDescriptionForFucktory() {
	static PRM_Name radius_name("radius", "Radius");
	static PRM_Name iterations_name("iterations", "Constraint Iterations Count");
	static PRM_Name substeps_name("substeps", "Substeps Count");
	static PRM_Name maxSpeed_name("maxSpeed", "Maximum Particle Speed");
	static PRM_Name maxAcceleration_name("maxAcceleration", "Maximum Particle Acceleration");

	static PRM_Name fluidRestDistanceMult_name("fluidRestDistanceMult", "Rest Distance Multiplier");
	static PRM_Name planesCount_name("planesCount", "Planes Count");
	static PRM_Name adhesion_name("adhesion", "Adhesion");
	static PRM_Name cohesion_name("cohesion", "Cohesion");
	static PRM_Name surfaceTension_name("surfaceTension", "Surface Tension");
	static PRM_Name viscosity_name("viscosity", "Viscosity");
	static PRM_Name relaxationFactor_name("relaxationFactor", "Relaxation Factor");
	static PRM_Name solidPressure_name("solidPressure", "Solid Pressure");
	static PRM_Name vorticityConfinement_name("vorticityConfinement", "Vorticity Confinement");
	static PRM_Name buoyancy_name("buoyancy", "Buoyancy");

	static PRM_Name dynamicfriction_name("dynamicfriction", "Dynamic Friction");
	static PRM_Name staticfriction_name("staticfriction", "Static Friction");
	static PRM_Name particleFriction_name("particleFriction", "Particle Friction");
	static PRM_Name drag_name("drag", "Cloth Drag");
	static PRM_Name lift_name("lift", "Cloth Lift");

	static PRM_Name shapeCollisionMargin_name("shapeCollisionMargin", "Shape Collision Margin");
	static PRM_Name particleCollisionMargin_name("particleCollisionMargin", "Particle Collision Margin");
	static PRM_Name collisionDistance_name("collisionDistance", "Collision Distance");
	

	static PRM_Default radius_default(0.1f);
	static PRM_Default iterations_default(3);
	static PRM_Default substeps_default(6);
	static PRM_Default maxSpeed_default(FLT_MAX);
	static PRM_Default maxAcceleration_default(1000.0f);
	static PRM_Default fluidRestDistanceMult_defaults(0.55f);
	static PRM_Default planesCount_defaults(5);
	static PRM_Default adhesion_defaults(0.0f);
	static PRM_Default cohesion_defaults(0.025f);
	static PRM_Default surfaceTension_defaults(0.0f);
	static PRM_Default viscosity_defaults(0.0f);
	static PRM_Default relaxationFactor_default(1.0f);
	static PRM_Default solidPressure_default(0.1f);
	static PRM_Default vorticityConfinement_default(0.0f);
	static PRM_Default buoyancy_default(1.0f);

	static PRM_Default dynamicfriction_defaults(0.0f);
	static PRM_Default staticfriction_defaults(0.0f);
	static PRM_Default particleFriction_defaults(0.0f);
	static PRM_Default shapeCollisionMargin_defaults(0.05f);
	static PRM_Default particleCollisionMargin_defaults(0.0f);
	static PRM_Default collisionDistance_defaults(0.0275f);

	static PRM_Default zero_defaults(0.0f);

	static PRM_Range iterations_range(PRM_RANGE_RESTRICTED, 1, PRM_RANGE_UI, 16);
	static PRM_Range substeps_range(PRM_RANGE_RESTRICTED, 1, PRM_RANGE_UI, 16);
	static PRM_Range maxSpeed_range(PRM_RANGE_RESTRICTED, 0, PRM_RANGE_UI, FLT_MAX);
	static PRM_Range maxAcceleration_range(PRM_RANGE_RESTRICTED, 0, PRM_RANGE_UI, 1000);
	static PRM_Range planesCount_range(PRM_RANGE_RESTRICTED, 0, PRM_RANGE_RESTRICTED, 5);

	static PRM_Range zeroOne_range(PRM_RANGE_RESTRICTED, 0, PRM_RANGE_UI, 1.0f);


	//seps
	static PRM_Name sep0("sep0", "sep0");
	static PRM_Name sep1("sep1", "sep1");
	static PRM_Name sep2("sep2", "sep2");
	static PRM_Name sep3("sep3", "sep3");
	//endseps

	static PRM_Template prms[] = {
		PRM_Template(PRM_FLT, 1, &radius_name, &radius_default),
		PRM_Template(PRM_INT, 1, &iterations_name, &iterations_default, 0, &iterations_range),
		PRM_Template(PRM_INT, 1, &substeps_name, &substeps_default, 0, &substeps_range),
		PRM_Template(PRM_FLT_LOG, 1, &maxSpeed_name, &maxSpeed_default, 0, &maxSpeed_range),
		PRM_Template(PRM_FLT, 1, &maxAcceleration_name, &maxAcceleration_default, 0, &maxAcceleration_range),
		PRM_Template(PRM_SEPARATOR, 1, &sep0),
		PRM_Template(PRM_FLT, 1, &fluidRestDistanceMult_name, &fluidRestDistanceMult_defaults),
		PRM_Template(PRM_INT, 1, &planesCount_name,&planesCount_defaults,0,&planesCount_range),
		PRM_Template(PRM_SEPARATOR, 1, &sep1),
		PRM_Template(PRM_FLT, 1, &adhesion_name, &adhesion_defaults),
		PRM_Template(PRM_FLT, 1, &cohesion_name, &cohesion_defaults),
		PRM_Template(PRM_FLT, 1, &surfaceTension_name, &surfaceTension_defaults),
		PRM_Template(PRM_FLT, 1, &viscosity_name, &viscosity_defaults),
		PRM_Template(PRM_FLT, 1, &relaxationFactor_name, &relaxationFactor_default),
		PRM_Template(PRM_FLT, 1, &solidPressure_name, &solidPressure_default),
		PRM_Template(PRM_FLT, 1, &vorticityConfinement_name, &vorticityConfinement_default),
		PRM_Template(PRM_FLT, 1, &buoyancy_name, &buoyancy_default),
		PRM_Template(PRM_SEPARATOR, 1, &sep2),
		PRM_Template(PRM_FLT, 1, &dynamicfriction_name, &dynamicfriction_defaults),
		PRM_Template(PRM_FLT, 1, &staticfriction_name, &staticfriction_defaults),
		PRM_Template(PRM_FLT, 1, &particleFriction_name, &particleFriction_defaults),
		PRM_Template(PRM_FLT, 1, &drag_name, &zero_defaults, 0, &zeroOne_range),
		PRM_Template(PRM_FLT, 1, &lift_name, &zero_defaults, 0, &zeroOne_range),
		PRM_Template(PRM_SEPARATOR, 1, &sep3),
		PRM_Template(PRM_FLT, 1, &shapeCollisionMargin_name, &shapeCollisionMargin_defaults),
		PRM_Template(PRM_FLT, 1, &particleCollisionMargin_name, &particleCollisionMargin_defaults),
		PRM_Template(PRM_FLT, 1, &collisionDistance_name, &collisionDistance_defaults),
		PRM_Template()
	};

	static SIM_DopDescription desc(true, "nvflexSolver", "NvFlex Solver", "Solver", classname(), prms);
	return &desc;
}


SIM_NvFlexSolver::SIM_NvFlexSolver(const SIM_DataFactory * fack) :SIM_Solver(fack), SIM_OptionsUser(this){}

SIM_NvFlexSolver::~SIM_NvFlexSolver(){}
/**
 * This file is part of DSO.
 *
 * Copyright 2016 Technical University of Munich and Intel.
 * Developed by Jakob Engel <engelj at in dot tum dot de>,
 * for more information see <http://vision.in.tum.de/dso>.
 * If you use this code, please cite the respective publications as
 * listed on the above website.
 *
 * DSO is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DSO is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DSO. If not, see <http://www.gnu.org/licenses/>.
 */



#include "FullSystem/ImmaturePoint.h"
#include "util/FrameShell.h"
#include "FullSystem/ResidualProjections.h"

namespace dso {

/// here u_, v_ is added by 0.5
ImmaturePoint::ImmaturePoint(int u_, int v_, FrameHessian* host_, float type, CalibHessian* HCalib)
    : u(u_), v(v_), host(host_), my_type(type), idepth_min(0), idepth_max(NAN), lastTraceStatus(IPS_UNINITIALIZED)
{

  gradH.setZero();  //Mat22f gradH

  for(int idx=0;idx<patternNum;idx++)
  {
    int dx = patternP[idx][0];
    int dy = patternP[idx][1];

    /// integration due to +0.5, interpolation results in 3 values [pixel, dx, dy]
    Vec3f ptc = getInterpolatedElement33BiLin(host->dI, u+dx, v+dy,wG[0]);


    color[idx] = ptc[0];
    if(!std::isfinite(color[idx])) {energyTH=NAN; return;}

    /// gradient matrix [dx^{2}, dxdy, dydx, dy^{2}]
    gradH += ptc.tail<2>()  * ptc.tail<2>().transpose();

    /// weight of points c^{2} / (c^{2} + || grad || ^{2})
    weights[idx] = sqrtf(setting_outlierTHSumComponent / (setting_outlierTHSumComponent + ptc.tail<2>().squaredNorm()));
  }

  energyTH = patternNum * setting_outlierTH;
  energyTH *= setting_overallEnergyTHWeight * setting_overallEnergyTHWeight;

  quality=10000;
}

ImmaturePoint::ImmaturePoint(float u_, float v_, FrameHessian* host_, CalibHessian* HCalib)
    : u(u_), v(v_), host(host_), idepth_min(0), idepth_max(NAN), lastTraceStatus(IPS_UNINITIALIZED)
{
  gradH.setZero();  //Mat22f gradH

  for(int idx=0;idx<patternNum;idx++)
  {
    int dx = patternP[idx][0];
    int dy = patternP[idx][1];

    Vec3f ptc = getInterpolatedElement33BiLin(host->dI, u+dx, v+dy,wG[0]);

    color[idx] = ptc[0];
    if(!std::isfinite(color[idx])) {energyTH=NAN; return;}

    gradH += ptc.tail<2>()  * ptc.tail<2>().transpose();

    weights[idx] = sqrtf(setting_outlierTHSumComponent / (setting_outlierTHSumComponent + ptc.tail<2>().squaredNorm()));
  }

  energyTH = patternNum*setting_outlierTH;
  energyTH *= setting_overallEnergyTHWeight*setting_overallEnergyTHWeight;

  quality=10000;
}

ImmaturePoint::~ImmaturePoint()
{}

// do static stereo match. if mode_right = true, it matches from left to right. otherwise do it from right to left.
ImmaturePointStatus ImmaturePoint::traceStereo(FrameHessian* frame,
                                               Mat33f K,
                                               bool mode_right) {
  // KRKi
  Mat33f KRKi = Mat33f::Identity().cast<float>();
  // Kt
  Vec3f Kt;
  // T between stereo cameras
  Vec3f bl;

  if(mode_right) {
    bl << -baseline, 0, 0;
  }
  else {
    bl << baseline, 0, 0;
  }

  Kt = K*bl;
  // to simplify set aff 1, 0
  Vec2f aff;
  aff << 1, 0;

  // baseline * fx
  float bf = -K(0,0)*bl[0];

  Vec3f pr = KRKi * Vec3f(u_stereo, v_stereo, 1);
  Vec3f ptpMin = pr + Kt*idepth_min_stereo;

  float uMin = ptpMin[0] / ptpMin[2];
  float vMin = ptpMin[1] / ptpMin[2];

  if(!(uMin > 4 && vMin > 4 && uMin < wG[0]-5 && vMin < hG[0]-5))
  {
    lastTraceUV = Vec2f(-1,-1);
    lastTracePixelInterval=0;
    return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
  }

  float dist;
  float uMax;
  float vMax;
  Vec3f ptpMax;
  float maxPixSearch = (wG[0]+hG[0])*setting_maxPixSearch;

  if(std::isfinite(idepth_max_stereo))
  {
    ptpMax = pr + Kt*idepth_max_stereo;
    uMax = ptpMax[0] / ptpMax[2];
    vMax = ptpMax[1] / ptpMax[2];


    if(!(uMax > 4 && vMax > 4 && uMax < wG[0]-5 && vMax < hG[0]-5))
    {
      lastTraceUV = Vec2f(-1,-1);
      lastTracePixelInterval=0;
      return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
    }

    // ============== check their distance. everything below 2px is OK (-> skip). ===================
    dist = (uMin-uMax)*(uMin-uMax) + (vMin-vMax)*(vMin-vMax);
    dist = sqrtf(dist);
    if(dist < setting_trace_slackInterval)
    {
      //				lastTraceUV_Stereo = Vec2f(uMax+uMin, vMax+vMin)*0.5;
      //				lastTracePixelInterval_Stereo=dist;
      //				idepth_stereo = (u_stereo - 0.5*(uMax+uMin))/bf;
      //				return lastTraceStatus_Stereo = ImmaturePointStatus::IPS_SKIPPED;
      return lastTraceStatus = ImmaturePointStatus ::IPS_SKIPPED;

    }
    assert(dist>0);
  }
  else
  {
    dist = maxPixSearch;

    // project to arbitrary depth to get direction.
    ptpMax = pr + Kt*0.01;
    uMax = ptpMax[0] / ptpMax[2];
    vMax = ptpMax[1] / ptpMax[2];

    // direction.
    float dx = uMax-uMin;
    float dy = vMax-vMin;
    float d = 1.0f / sqrtf(dx*dx+dy*dy);

    // set to [setting_maxPixSearch].
    uMax = uMin + dist*dx*d;
    vMax = vMin + dist*dy*d;

    // may still be out!
    if(!(uMax > 4 && vMax > 4 && uMax < wG[0]-5 && vMax < hG[0]-5))
    {
      lastTraceUV = Vec2f(-1,-1);
      lastTracePixelInterval=0;
      return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
    }
    assert(dist>0);
  }

  //		 set OOB if scale change too big.
  if(!(idepth_min<0 || (ptpMin[2]>0.75 && ptpMin[2]<1.5)))
  {
    lastTraceUV = Vec2f(-1, -1);
    lastTracePixelInterval = 0;
    return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
  }

  // ============== compute error-bounds on result in pixel. if the new interval is not at least 1/2 of the old, SKIP ===================
  float dx = setting_trace_stepsize*(uMax-uMin);
  float dy = setting_trace_stepsize*(vMax-vMin);

  float a = (Vec2f(dx,dy).transpose() * gradH * Vec2f(dx,dy));
  float b = (Vec2f(dy,-dx).transpose() * gradH * Vec2f(dy,-dx));
  float errorInPixel = 0.2f + 0.2f * (a+b) / a;

  // std::cout << "dx: " << dx << ", dy: " << dy << ", a: " << a << ", b: " << b << ", errorInPixel: " << errorInPixel << std::endl;

  if(errorInPixel*setting_trace_minImprovementFactor > dist && std::isfinite(idepth_max_stereo))
  {
    //			lastTraceUV_Stereo = Vec2f(uMax+uMin, vMax+vMin)*0.5;
    //			lastTracePixelInterval_Stereo=dist;
    //			idepth_stereo = (u_stereo - 0.5*(uMax+uMin))/bf;
    //			return lastTraceStatus_Stereo = ImmaturePointStatus::IPS_BADCONDITION;
    //            lastTraceUV = Vec2f(u, v);
    //            lastTracePixelInterval = dist;
    return lastTraceStatus = ImmaturePointStatus ::IPS_BADCONDITION;
  }

  if(errorInPixel >10) errorInPixel=10;

  // ============== do the discrete search ===================
  dx /= dist;
  dy /= dist;

  // std::cout << "dx2: " << dx << ", dy2: " << dy << ", dist: " << dist << std::endl << std::endl;

  if(dist>maxPixSearch)
  {
    uMax = uMin + maxPixSearch*dx;
    vMax = vMin + maxPixSearch*dy;
    dist = maxPixSearch;
  }

  int numSteps = 1.9999f + dist / setting_trace_stepsize;
  Mat22f Rplane = KRKi.topLeftCorner<2,2>();

  float randShift = uMin*1000-floorf(uMin*1000);
  float ptx = uMin-randShift*dx;
  float pty = vMin-randShift*dy;


  Vec2f rotatetPattern[MAX_RES_PER_POINT];
  for(int idx=0;idx<patternNum;idx++)
    rotatetPattern[idx] = Rplane * Vec2f(patternP[idx][0], patternP[idx][1]);

  if(!std::isfinite(dx) || !std::isfinite(dy))
  {
    lastTraceUV = Vec2f(-1,-1);
    lastTracePixelInterval=0;
    return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
  }

  float errors[100];
  float bestU=0, bestV=0, bestEnergy=1e10;
  int bestIdx=-1;
  if(numSteps >= 100) numSteps = 99;

  for(int i=0;i<numSteps;i++) {
    float energy=0;
    for(int idx=0;idx<patternNum;idx++) {

      float hitColor = getInterpolatedElement31(frame->dI,
                                                (float)(ptx+rotatetPattern[idx][0]),
                                                (float)(pty+rotatetPattern[idx][1]),
                                                wG[0]);

      if(!std::isfinite(hitColor)) {
        energy+=1e5;
        continue;
      }

      float residual = hitColor - (float)(aff[0] * color[idx] + aff[1]);
      float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);
      energy += hw *residual*residual*(2-hw);
    }

    errors[i] = energy;
    if(energy < bestEnergy)
    {
      bestU = ptx;
      bestV = pty;
      bestEnergy = energy;
      bestIdx = i;
    }

    ptx+=dx;
    pty+=dy;
  }

  // find best score outside a +-2px radius.
  float secondBest=1e10;
  for(int i=0;i<numSteps;i++) {
    if((i < bestIdx-setting_minTraceTestRadius || i > bestIdx+setting_minTraceTestRadius) && errors[i] < secondBest) {
      secondBest = errors[i];
    }
  }

  float newQuality = secondBest / bestEnergy;
  if(newQuality < quality || numSteps > 10) {
    quality = newQuality;
  }


  // ============== do GN optimization ===================
  auto linear_solver = g2o::make_unique<g2o::LinearSolverEigen<g2o::BlockSolverX::PoseMatrixType>>();
  auto block_solver = g2o::make_unique<g2o::BlockSolverX>(std::move(linear_solver));
  auto algorithm = new g2o::OptimizationAlgorithmGaussNewton(std::move(block_solver));
  g2o::SparseOptimizer* optimizer = new g2o::SparseOptimizer();
  optimizer->setAlgorithm(algorithm);

  //// set vertex.
  VertexUVDSO* vtx_uv = new VertexUVDSO();
  vtx_uv->setEstimate(Vec2(bestU, bestV));
  vtx_uv->setId(0);
  vtx_uv->SetDxDy(dx, dy);
  optimizer->addVertex(vtx_uv);

  // float uBak=bestU, vBak=bestV, gnstepsize=1, stepBack=0;

  if(setting_trace_GNIterations>0) {
    bestEnergy = 1e5;
  }

  int gnStepsGood=0, gnStepsBad=0;

  for(int it=0;it<setting_trace_GNIterations;it++) {
    // float H = 1, b=0;
    float energy=0;
    for(int idx=0;idx<patternNum;idx++) {
      EdgeTracePointUVDSO* edge = new EdgeTracePointUVDSO(aff, frame->dI, dx, dy, rotatetPattern[idx]);
      edge->setVertex(0, vtx_uv);
      edge->setMeasurement(color[idx]);
      edge->setInformation(Eigen::Matrix<double,1,1>::Identity());
      g2o::RobustKernelHuber* huber = new g2o::RobustKernelHuber;
      huber->setDelta(setting_huberTH);
      edge->setRobustKernel(huber);
      edge->setId(idx);

      edge->computeError();
      float residual = edge->error()(0);
      float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);
      energy += weights[idx]*weights[idx]*hw *residual*residual*(2-hw);

      optimizer->addEdge(edge);

      // Vec3f hitColor = getInterpolatedElement33(frame->dI,
      //                                           (float)(bestU+rotatetPattern[idx][0]),
      //                                           (float)(bestV+rotatetPattern[idx][1]),
      //                                           wG[0]);

      // if(!std::isfinite((float)hitColor[0])) {
      //   energy+=1e5;
      //   continue;
      // }

      // float residual = hitColor[0] - (aff[0] * color[idx] + aff[1]);
      // float dResdDist = dx*hitColor[1] + dy*hitColor[2];
      // float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);

      // H += hw*dResdDist*dResdDist;
      // b += hw*residual*dResdDist;
      // energy += weights[idx]*weights[idx]*hw *residual*residual*(2-hw);
    }

    optimizer->initializeOptimization();
    optimizer->setVerbose(false);
    // std::cout << "[*] traceStereo(): optimizing... " << std::endl;
    optimizer->optimize(1);

    if(energy > bestEnergy) {
      gnStepsBad++;

      // do a smaller step from old point.
      // stepBack*=0.5;
      // bestU = uBak + stepBack*dx;
      // bestV = vBak + stepBack*dy;
    }
    else {
      gnStepsGood++;

      // float step = -gnstepsize*b/H;
      // if(step < -0.5) {
      //   step = -0.5;
      // }
      // else if(step > 0.5) {
      //   step=0.5;
      // }

      // if(!std::isfinite(step)) {
      //   step=0;
      // }

      // uBak=bestU;
      // vBak=bestV;
      // stepBack=step;

      // bestU += step*dx;
      // bestV += step*dy;
      bestEnergy = energy;

    }

    // if(fabsf(stepBack) < setting_trace_GNThreshold) break;
  }

  bestU = vtx_uv->estimate()(0);
  bestV = vtx_uv->estimate()(1);

  if(!(bestEnergy < energyTH*setting_trace_extraSlackOnTH))
  {

    lastTracePixelInterval=0;
    lastTraceUV = Vec2f(-1,-1);
    if(lastTraceStatus == ImmaturePointStatus::IPS_OUTLIER)
      return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
    else
      return lastTraceStatus = ImmaturePointStatus::IPS_OUTLIER;
  }

  // ============== set new interval ===================
  if(dx*dx > dy*dy) {
    idepth_min_stereo = (pr[2]*(bestU - errorInPixel*dx) - pr[0]) / (Kt[0] - Kt[2]*(bestU - errorInPixel*dx));
    idepth_max_stereo = (pr[2]*(bestU + errorInPixel*dx) - pr[0]) / (Kt[0] - Kt[2]*(bestU + errorInPixel*dx));
  }
  else {
    idepth_min_stereo = (pr[2]*(bestV - errorInPixel*dy) - pr[1]) / (Kt[1] - Kt[2]*(bestV - errorInPixel*dy));
    idepth_max_stereo = (pr[2]*(bestV + errorInPixel*dy) - pr[1]) / (Kt[1] - Kt[2]*(bestV + errorInPixel*dy));
  }

  if(idepth_min_stereo > idepth_max_stereo) {
    std::swap<float>(idepth_min_stereo, idepth_max_stereo);
  }

  //  printf("the idpeth_min is %f, the idepth_max is %f \n", idepth_min, idepth_max);

  if(!std::isfinite(idepth_min_stereo) || !std::isfinite(idepth_max_stereo) || (idepth_max_stereo<0)) {
    lastTracePixelInterval=0;
    lastTraceUV = Vec2f(-1,-1);
    return lastTraceStatus = ImmaturePointStatus::IPS_OUTLIER;
  }

  lastTracePixelInterval=2*errorInPixel;
  lastTraceUV = Vec2f(bestU, bestV);
  idepth_stereo = (u_stereo - bestU)/bf;

  return lastTraceStatus = ImmaturePointStatus::IPS_GOOD;
}

/*
 * returns
 * * OOB -> point is optimized and marginalized
 * * UPDATED -> point has been updated.
 * * SKIP -> point has not been updated.
 */
ImmaturePointStatus ImmaturePoint::traceOn(FrameHessian* frame,
                                           Mat33f hostToFrame_KRKi,
                                           Vec3f hostToFrame_Kt,
                                           Vec2f hostToFrame_affine,
                                           CalibHessian* HCalib,
                                           bool debugPrint) {

  if(lastTraceStatus == ImmaturePointStatus::IPS_OOB) {
    return lastTraceStatus;
  }

  debugPrint = false;//rand()%100==0;
  float maxPixSearch = (wG[0]+hG[0])*setting_maxPixSearch;

  if(debugPrint) {
    printf("trace pt (%.1f %.1f) from frame %d to %d. Range %f -> %f. t %f %f %f!\n",
           u,v,
           host->shell->id, frame->shell->id,
           idepth_min, idepth_max,
           hostToFrame_Kt[0],hostToFrame_Kt[1],hostToFrame_Kt[2]);
  }

  // const float stepsize = 1.0;	// stepsize for initial discrete search.
  // const int GNIterations = 3;	// max # GN iterations
  // const float GNThreshold = 0.1;	// GN stop after this stepsize.
  // const float extraSlackOnTH = 1.2;	// for energy-based outlier check, be slightly more relaxed by this factor.
  // const float slackInterval = 0.8;	// if pixel-interval is smaller than this, leave it be.
  // const float minImprovementFactor = 2;	// if pixel-interval is smaller than this, leave it be.

  /// STEP1: calculate the upper and lower limits of the search, corresponding to idepth_max, idepth_min.
  // ============== project min and max. return if one of them is OOB ===================
  Vec3f pr = hostToFrame_KRKi * Vec3f(u,v, 1);
  Vec3f ptpMin = pr + hostToFrame_Kt * idepth_min;

  float uMin = ptpMin[0] / ptpMin[2];
  float vMin = ptpMin[1] / ptpMin[2];

  // float idepth_min_project = 1.0f / ptpMin[2];
  // printf("the idepth min project %f \n", idepth_min_project);

  /// if it exceeds the image range, set it to OOB.
  if(!(uMin > 4 && vMin > 4 && uMin < wG[0]-5 && vMin < hG[0]-5)) {
    if(debugPrint) printf("OOB uMin %f %f - %f %f %f (id %f-%f)!\n",
                          u,v,uMin, vMin,  ptpMin[2], idepth_min, idepth_max);
    lastTraceUV = Vec2f(-1,-1);
    lastTracePixelInterval=0;
    return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
  }

  float dist;
  float uMax;
  float vMax;
  Vec3f ptpMax;

  if(std::isfinite(idepth_max)) {
    ptpMax = pr + hostToFrame_Kt*idepth_max;
    uMax = ptpMax[0] / ptpMax[2];
    vMax = ptpMax[1] / ptpMax[2];

    if(!(uMax > 4 && vMax > 4 && uMax < wG[0]-5 && vMax < hG[0]-5)) {
      if(debugPrint) printf("OOB uMax  %f %f - %f %f!\n",u,v, uMax, vMax);
      lastTraceUV = Vec2f(-1,-1);
      lastTracePixelInterval=0;
      return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
    }

    // ============== check their distance. everything below 2px is OK (-> skip). ===================
    dist = (uMin-uMax)*(uMin-uMax) + (vMin-vMax)*(vMin-vMax);
    dist = sqrtf(dist);

    /// the search range is too small.
    if(dist < setting_trace_slackInterval) {
      if(debugPrint)
        printf("TOO CERTAIN ALREADY (dist %f)!\n", dist);

      lastTraceUV = Vec2f(uMax+uMin, vMax+vMin)*0.5; /// directly set to median.
      lastTracePixelInterval=dist;
      return lastTraceStatus = ImmaturePointStatus::IPS_SKIPPED; /// skip
    }
    assert(dist > 0);
  }
  else {
    /// the upper limit is infinite, then set to the maximum value.
    dist = maxPixSearch;

    // project to arbitrary depth to get direction.
    ptpMax = pr + hostToFrame_Kt*0.01;
    uMax = ptpMax[0] / ptpMax[2];
    vMax = ptpMax[1] / ptpMax[2];

    // direction.
    float dx = uMax-uMin;
    float dy = vMax-vMin;
    float d = 1.0f / sqrtf(dx*dx+dy*dy);

    /// get the maximum value according to the proportion.
    // set to [setting_maxPixSearch].
    uMax = uMin + dist*dx*d;
    vMax = vMin + dist*dy*d;

    // may still be out!
    if(!(uMax > 4 && vMax > 4 && uMax < wG[0]-5 && vMax < hG[0]-5)) {
      if(debugPrint) printf("OOB uMax-coarse %f %f %f!\n", uMax, vMax,  ptpMax[2]);
      lastTraceUV = Vec2f(-1,-1);
      lastTracePixelInterval=0;
      return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
    }
    assert(dist>0);
  }


  /// ? why is this value ??? 0.75-1.5
  /// this value is the ratio of the depth on the two frames, if it changes too much, it will change too much before and after the scale.
  // set OOB if scale change too big.
  if(!(idepth_min < 0 || (ptpMin[2]>0.75 && ptpMin[2]<1.5)))
  {
    if(debugPrint) printf("OOB SCALE %f %f %f!\n", uMax, vMax,  ptpMin[2]);
    lastTraceUV = Vec2f(-1,-1);
    lastTracePixelInterval=0;
    return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
  }


  /// STEP2: calculate the magnitude of the error. (the angle b/w the image gradient and the epipolar line)
  // ============== compute error-bounds on result in pixel. if the new interval is not at least 1/2 of the old, SKIP ===================
  float dx = setting_trace_stepsize*(uMax-uMin);
  float dy = setting_trace_stepsize*(vMax-vMin);

  /// (dIx * dx + dIy * dy) ^{2}
  float a = (Vec2f(dx,dy).transpose() * gradH * Vec2f(dx,dy));
  /// (dIx * dy - dIy * dx) ^{2}
  float b = (Vec2f(dy,-dx).transpose() * gradH * Vec2f(dy,-dx)); /// (dx,dy) product in  vertical direction.

  /// calculate the angle b/w the polar direction and the gradient direction, a=0 to 90 degrees, and errorInPixel becomes larger; b =0 when parallel a float errorInPixel=0...2F + 0...2F*(a+b)/a;
  /// do not use the LSD method that is estimated to be effective to prevent small displacements of the case.
  float errorInPixel = 0.2f + 0.2f * (a+b) / a;

  /// errorInPixel indicates that it is vertical. At this time, the error will be very large, which is regarded as bad.
  if(errorInPixel*setting_trace_minImprovementFactor > dist && std::isfinite(idepth_max))
  {
    if(debugPrint)
      printf("NO SIGNIFICANT IMPROVMENT (%f)!\n", errorInPixel);
    lastTraceUV = Vec2f(uMax+uMin, vMax+vMin)*0.5;
    lastTracePixelInterval=dist;
    return lastTraceStatus = ImmaturePointStatus::IPS_BADCONDITION;
  }

  if(errorInPixel >10) errorInPixel=10;

  /// STEP3: find the position of the smallest photometric error on the polar line, and calculate the ratio with the second time as the quailty
  // ============== do the discrete search ===================
  dx /= dist;   /// cos
  dy /= dist;   /// sin

  if(debugPrint)
    printf("trace pt (%.1f %.1f) from frame %d to %d. Range %f (%.1f %.1f) -> %f (%.1f %.1f)! ErrorInPixel %.1f!\n",
           u,v,
           host->shell->id, frame->shell->id,
           idepth_min, uMin, vMin,
           idepth_max, uMax, vMax,
           errorInPixel
           );


  if(dist>maxPixSearch)
  {
    uMax = uMin + maxPixSearch*dx;
    vMax = vMin + maxPixSearch*dy;
    dist = maxPixSearch;
  }

  /// number of steps.
  int numSteps = 1.9999f + dist / setting_trace_stepsize;
  Mat22f Rplane = hostToFrame_KRKi.topLeftCorner<2,2>();

  /// take a random number after the decimal point ??
  float randShift = uMin*1000-floorf(uMin*1000);
  float ptx = uMin-randShift*dx;
  float pty = vMin-randShift*dy;

  /// pattern offset on new frame.
  Vec2f rotatetPattern[MAX_RES_PER_POINT];
  for(int idx=0;idx<patternNum;idx++)
    rotatetPattern[idx] = Rplane * Vec2f(patternP[idx][0], patternP[idx][1]);

  /// this statement is too much, learn to learn, consider it all
  if(!std::isfinite(dx) || !std::isfinite(dy)) {
    //printf("COUGHT INF / NAN dxdy (%f %f)!\n", dx, dx);

    lastTracePixelInterval=0;
    lastTraceUV = Vec2f(-1,-1);
    return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
  }

  /// search for the position with the smallest error along the level line.
  float errors[100];
  float bestU=0, bestV=0, bestEnergy=1e10;
  int bestIdx=-1;
  if(numSteps >= 100) numSteps = 99;

  for(int i=0;i<numSteps;i++) {
    float energy=0;
    for(int idx=0;idx<patternNum;idx++)
    {
      float hitColor = getInterpolatedElement31(frame->dI,
                                                (float)(ptx+rotatetPattern[idx][0]),
                                                (float)(pty+rotatetPattern[idx][1]),
                                                wG[0]);

      if(!std::isfinite(hitColor)) {energy+=1e5; continue;}
      float residual = hitColor - (float)(hostToFrame_affine[0] * color[idx] + hostToFrame_affine[1]);
      float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);
      energy += hw *residual*residual*(2-hw);
    }

    if(debugPrint)
      printf("step %.1f %.1f (id %f): energy = %f!\n",
             ptx, pty, 0.0f, energy);


    errors[i] = energy;
    if(energy < bestEnergy)
    {
      bestU = ptx;
      bestV = pty;
      bestEnergy = energy;
      bestIdx = i;
    }

    /// each dist corresponding size.
    ptx+=dx;
    pty+=dy;
  }

  /// find the second samllest error within a certain radius, and the difference is large enough to be better (this is commonly used).
  // find best score outside a +-2px radius.
  float secondBest=1e10;
  for(int i=0;i<numSteps;i++)
  {
    if((i < bestIdx-setting_minTraceTestRadius || i > bestIdx+setting_minTraceTestRadius) && errors[i] < secondBest)
      secondBest = errors[i];
  }
  float newQuality = secondBest / bestEnergy;
  if(newQuality < quality || numSteps > 10) quality = newQuality;


  /// STEP4: perform a linear search at the optimal position above for refinement.
  // ============== do GN optimization ===================
  float uBak=bestU, vBak=bestV, gnstepsize=1, stepBack=0;
  if(setting_trace_GNIterations>0) bestEnergy = 1e5;
  int gnStepsGood=0, gnStepsBad=0;

  for(int it=0;it<setting_trace_GNIterations;it++) {
    float H = 1, b=0, energy=0;
    for(int idx=0;idx<patternNum;idx++) {
      Vec3f hitColor = getInterpolatedElement33(frame->dI,
                                                (float)(bestU+rotatetPattern[idx][0]),
                                                (float)(bestV+rotatetPattern[idx][1]),wG[0]);

      if(!std::isfinite((float)hitColor[0])) {energy+=1e5; continue;}
      float residual = hitColor[0] - (hostToFrame_affine[0] * color[idx] + hostToFrame_affine[1]);
      float dResdDist = dx*hitColor[1] + dy*hitColor[2];
      float hw = fabs(residual) < setting_huberTH ? 1 : setting_huberTH / fabs(residual);

      H += hw*dResdDist*dResdDist;
      b += hw*residual*dResdDist;
      energy += weights[idx]*weights[idx]*hw *residual*residual*(2-hw);
    }


    if(energy > bestEnergy)
    {
      gnStepsBad++;

      // do a smaller step from old point.
      stepBack*=0.5;  /// reduce the step size before calculating.
      bestU = uBak + stepBack*dx;
      bestV = vBak + stepBack*dy;
      if(debugPrint)
        printf("GN BACK %d: E %f, H %f, b %f. id-step %f. UV %f %f -> %f %f.\n",
               it, energy, H, b, stepBack,
               uBak, vBak, bestU, bestV);
    }
    else
    {
      gnStepsGood++;

      float step = -gnstepsize*b/H;

      /// the maximum step size is only 0.5
      if(step < -0.5) step = -0.5;
      else if(step > 0.5) step=0.5;

      if(!std::isfinite(step)) step=0;

      uBak=bestU;
      vBak=bestV;
      stepBack=step;

      bestU += step*dx;
      bestV += step*dy;
      bestEnergy = energy;

      if(debugPrint)
        printf("GN step %d: E %f, H %f, b %f. id-step %f. UV %f %f -> %f %f.\n",
               it, energy, H, b, step,
               uBak, vBak, bestU, bestV);
    }

    if(fabsf(stepBack) < setting_trace_GNThreshold) break;
  }

  // ============== detect energy-based outlier. ===================
  //float absGrad0 = getInterpolatedElement(frame->absSquaredGrad[0],bestU, bestV, wG[0]);
  //float absGrad1 = getInterpolatedElement(frame->absSquaredGrad[1],bestU*0.5-0.25, bestV*0.5-0.25, wG[1]);
  //float absGrad2 = getInterpolatedElement(frame->absSquaredGrad[2],bestU*0.25-0.375, bestV*0.25-0.375, wG[2]);

  /// residual is too large, then set to oulier
  if(!(bestEnergy < energyTH*setting_trace_extraSlackOnTH))
    //|| (absGrad0*areaGradientSlackFactor < host->frameGradTH
    //&& absGrad1*areaGradientSlackFactor < host->frameGradTH*0.75f
    //&& absGrad2*areaGradientSlackFactor < host->frameGradTH*0.50f))
  {
    if(debugPrint)
      printf("OUTLIER!\n");

    lastTracePixelInterval=0;
    lastTraceUV = Vec2f(-1,-1);
    /// ? there is sill a chance for oulier to change back?
    if(lastTraceStatus == ImmaturePointStatus::IPS_OUTLIER)
      return lastTraceStatus = ImmaturePointStatus::IPS_OOB;
    else
      return lastTraceStatus = ImmaturePointStatus::IPS_OUTLIER;
  }

  /// STEP5: re-calculate the range of inverse depth based on the obtained optimal position.
  // ============== set new interval ===================
  /// u = (pr[0] + Kt[0] * idepth) / (pr[2] + Kt[2] * idepth) ==> idepth = (u * pr[2] - pr[0]) / (Kt[0] - u*Kt[2])
  /// v = (pr[1] + Kt[1] * idepth) / (pr[2] + Kt[2] * idepth) ==> idepth = (v * pr[2] - pr[1]) / (Kt[1] - v*Kt[2])
  /// take the largest error
  if(dx*dx > dy*dy) {
    idepth_min = (pr[2]*(bestU-errorInPixel*dx) - pr[0]) / (hostToFrame_Kt[0] - hostToFrame_Kt[2]*(bestU-errorInPixel*dx));
    idepth_max = (pr[2]*(bestU+errorInPixel*dx) - pr[0]) / (hostToFrame_Kt[0] - hostToFrame_Kt[2]*(bestU+errorInPixel*dx));
  }
  else {
    idepth_min = (pr[2]*(bestV-errorInPixel*dy) - pr[1]) / (hostToFrame_Kt[1] - hostToFrame_Kt[2]*(bestV-errorInPixel*dy));
    idepth_max = (pr[2]*(bestV+errorInPixel*dy) - pr[1]) / (hostToFrame_Kt[1] - hostToFrame_Kt[2]*(bestV+errorInPixel*dy));
  }

  if(idepth_min > idepth_max) {
    std::swap<float>(idepth_min, idepth_max);
  }

  if(!std::isfinite(idepth_min) || !std::isfinite(idepth_max) || (idepth_max<0)) {
    //printf("COUGHT INF / NAN minmax depth (%f %f)!\n", idepth_min, idepth_max);

    lastTracePixelInterval=0;
    lastTraceUV = Vec2f(-1,-1);
    return lastTraceStatus = ImmaturePointStatus::IPS_OUTLIER;
  }

  /// search range.
  lastTracePixelInterval=2*errorInPixel;

  /// the last position obtained last time.
  lastTraceUV = Vec2f(bestU, bestV);

  /// last position.
  return lastTraceStatus = ImmaturePointStatus::IPS_GOOD;
}

//// never used function.
float ImmaturePoint::getdPixdd(CalibHessian *  HCalib,
                               ImmaturePointTemporaryResidual* tmpRes,
                               float idepth) {

  FrameFramePrecalc* precalc = &(host->targetPrecalc[tmpRes->target->idx]);
  const Vec3f &PRE_tTll = precalc->PRE_tTll;
  float drescale, u=0, v=0, new_idepth;
  float Ku, Kv;
  Vec3f KliP;

  projectPoint(this->u,this->v, idepth, 0, 0,HCalib,
               precalc->PRE_RTll,PRE_tTll, drescale, u, v, Ku, Kv, KliP, new_idepth);

  float dxdd = (PRE_tTll[0]-PRE_tTll[2]*u)*HCalib->fxl();
  float dydd = (PRE_tTll[1]-PRE_tTll[2]*v)*HCalib->fyl();
  return drescale*sqrtf(dxdd*dxdd + dydd*dydd);
}

//// never used function.
float ImmaturePoint::calcResidual(CalibHessian *  HCalib, const float outlierTHSlack,
                                  ImmaturePointTemporaryResidual* tmpRes,
                                  float idepth) {

  FrameFramePrecalc* precalc = &(host->targetPrecalc[tmpRes->target->idx]);

  float energyLeft=0;
  const Eigen::Vector3f* dIl = tmpRes->target->dI;
  const Mat33f &PRE_KRKiTll = precalc->PRE_KRKiTll;
  const Vec3f &PRE_KtTll = precalc->PRE_KtTll;
  Vec2f affLL = precalc->PRE_aff_mode;

  for(int idx=0;idx<patternNum;idx++)
  {
    float Ku, Kv;
    if(!projectPoint(this->u+patternP[idx][0], this->v+patternP[idx][1], idepth, PRE_KRKiTll, PRE_KtTll, Ku, Kv))
    {return 1e10;}

    Vec3f hitColor = (getInterpolatedElement33(dIl, Ku, Kv, wG[0]));
    if(!std::isfinite((float)hitColor[0])) {return 1e10;}
    //if(benchmarkSpecialOption==5) hitColor = (getInterpolatedElement13BiCub(tmpRes->target->I, Ku, Kv, wG[0]));

    float residual = hitColor[0] - (affLL[0] * color[idx] + affLL[1]);

    float hw = fabsf(residual) < setting_huberTH ? 1 : setting_huberTH / fabsf(residual);
    energyLeft += weights[idx]*weights[idx]*hw *residual*residual*(2-hw);
  }

  if(energyLeft > energyTH*outlierTHSlack) {
    energyLeft = energyTH*outlierTHSlack;
  }

  return energyLeft;
}

/// calculate the residual of the idepth of the current point, the normal equations (H and b), the residual state
double ImmaturePoint::linearizeResidual(CalibHessian* HCalib, const float outlierTHSlack,
                                        ImmaturePointTemporaryResidual* tmpRes,
                                        float &Hdd, float &bd,
                                        float idepth,
                                        g2o::SparseOptimizer* optimizer,
                                        VertexInverseDepthDSO* vtx_idepth) {

  if(tmpRes->state_state == ResState::OOB) {
    tmpRes->state_NewState = ResState::OOB; return tmpRes->state_energy;
  }

  FrameFramePrecalc* precalc = &(host->targetPrecalc[tmpRes->target->idx]);

  // check OOB due to scale angle change.

  float energyLeft=0;
  const Eigen::Vector3f* dIl = tmpRes->target->dI;
  const Mat33f &PRE_RTll = precalc->PRE_RTll;
  const Vec3f &PRE_tTll = precalc->PRE_tTll;
  //const float * const Il = tmpRes->target->I;

  Vec2f affLL = precalc->PRE_aff_mode;

  for(int idx=0;idx<patternNum;idx++) {
    int dx = patternP[idx][0];
    int dy = patternP[idx][1];

    // float drescale, u, v, new_idepth;
    // float Ku, Kv;
    // Vec3f KliP;

    // make idepth edge.
    EdgePointActivationIdepthDSO* edge = new EdgePointActivationIdepthDSO(this->u+dx, this->v+dy, affLL, dIl, HCalib, PRE_RTll, PRE_tTll);
    edge->setVertex(0, vtx_idepth);
    edge->setMeasurement(color[idx]);
    edge->setInformation(Eigen::Matrix<double,1,1>::Identity());
    g2o::RobustKernelHuber* huber = new g2o::RobustKernelHuber;
    huber->setDelta(setting_huberTH);
    edge->setRobustKernel(huber);
    edge->setId(idx);

    //// project point to current frame.
    edge->projectPoint();

    edge->computeError();
    float residual = edge->error()(0);
    float hw = fabsf(residual) < setting_huberTH ? 1 : setting_huberTH / fabsf(residual);
    energyLeft += weights[idx]*weights[idx]*hw*residual*residual*(2-hw);

    if(edge->error()(0) > energyTH*outlierTHSlack/patternNum) {
      edge->setLevel(1);
    }

    // add idepth edge to optimizer.
    optimizer->addEdge(edge);

    // if(!projectPoint(this->u,this->v, idepth, dx, dy,HCalib,
    //                  PRE_RTll,PRE_tTll, drescale, u, v, Ku, Kv, KliP, new_idepth))
    // {
    //   tmpRes->state_NewState = ResState::OOB;
    //   return tmpRes->state_energy;
    // }

    // Vec3f hitColor = (getInterpolatedElement33(dIl, Ku, Kv, wG[0]));

    // if(!std::isfinite((float)hitColor[0])) {tmpRes->state_NewState = ResState::OOB; return tmpRes->state_energy;}
    // float residual = hitColor[0] - (affLL[0] * color[idx] + affLL[1]);

    // float hw = fabsf(residual) < setting_huberTH ? 1 : setting_huberTH / fabsf(residual);
    // energyLeft += weights[idx]*weights[idx]*hw *residual*residual*(2-hw);

    // // depth derivatives.
    // float dxInterp = hitColor[1]*HCalib->fxl();
    // float dyInterp = hitColor[2]*HCalib->fyl();

    // /// derivative of idepth.
    // float d_idepth = derive_idepth(PRE_tTll, u, v, dx, dy, dxInterp, dyInterp, drescale);

    // hw *= weights[idx]*weights[idx];

    // /// hessian for idepth.
    // Hdd += (hw*d_idepth)*d_idepth;

    // /// Jres for idepth.
    // bd += (hw*residual)*d_idepth;
  }

  if(energyLeft > energyTH*outlierTHSlack)
  {
    energyLeft = energyTH*outlierTHSlack;
    tmpRes->state_NewState = ResState::OUTLIER;
  }
  else
  {
    tmpRes->state_NewState = ResState::IN;
  }

  tmpRes->state_NewEnergy = energyLeft;
  return energyLeft;
}

} // namespace dso

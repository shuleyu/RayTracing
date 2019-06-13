#include<Ray.hpp>

#include<CreateGrid.hpp>
#include<LineJunction.hpp>
#include<LocDist.hpp>
#include<PointInPolygon.hpp>
#include<PREM.hpp>
#include<RayPath.hpp>
#include<SegmentJunction.hpp>
#include<PlaneWaveCoefficients.hpp>

using namespace std;

// Utilities for 1D-altering the PREM model.
vector<double> MakeRef(const double &depth,const vector<vector<double>> &dev){
    double rho=Drho(depth),vs=Dvs(depth),vp=Dvp(depth);
    for (const auto &item: dev) {
        if (item[0]<=depth && depth<=item[1]) {
            vp*=(1+item[2]/100);
            vs*=(1+item[3]/100);
            rho*=(1+item[4]/100);
            break;
        }
    }
    return {vp,vs,rho};
}

// Utilities for finding the index in an array that is cloeset to a given radius.
// array is sorted descending.
size_t findClosetLayer(const vector<double> &R, const double &r){
    auto rit=upper_bound(R.rbegin(),R.rend(),r);
    if (rit==R.rbegin()) return R.size()-1;
    else if (rit==R.rend()) return 0;
    else {
        auto it=rit.base();
        if (fabs(*it-r)<fabs(*rit-r)) return distance(R.begin(),it);
        else return distance(rit,R.rend())-1;
    }
}

// Utilities for finding the index in an array that is cloeset to a given depth.
// array is sorted ascending.
size_t findClosetDepth(const vector<double> &D, const double &d){
    auto it=upper_bound(D.begin(),D.end(),d);
    if (it==D.begin()) return 0;
    else if (it==D.end()) return D.size()-1;
    else {
        if (fabs(*it-d)<fabs(*prev(it)-d)) return distance(D.begin(),it);
        else return distance(D.begin(),it)-1;
    }
}

void PreprocessAndRun(
        const vector<int> &initRaySteps,const vector<int> &initRayComp,const vector<int> &initRayColor,
        const vector<double> &initRayTheta,const vector<double> &initRayDepth,const vector<double> &initRayTakeoff,
        const vector<double> &gridDepth1,const vector<double> &gridDepth2,const vector<double> &gridInc,
        const vector<double> &specialDepths,const vector<vector<double>> &Deviation,
        const vector<vector<double>> &regionProperties,
        const vector<vector<double>> &regionPolygonsTheta,
        const vector<vector<double>> &regionPolygonsDepth,
        const double &RectifyLimit, const bool &TS, const bool &TD, const bool &RS, const bool &RD,
        const size_t &nThread, const bool &DebugInfo, const bool &StopAtSurface,
        const size_t &branches, const size_t &potentialSize,
        char **ReachSurfaces, int *ReachSurfacesSize, char **RayInfo, int *RayInfoSize,
        int *RegionN,double **RegionsTheta,double **RegionsRadius,
        double **RaysTheta, int *RaysN, double **RaysRadius,int *Observer){

    // Ray nodes.
    vector<Ray> RayHeads;
    if (potentialSize>RayHeads.max_size()) {
        throw runtime_error("Too many rays to handle: decrease the number of legs or the number of input rays...");
    }

    // Create 1D reference layers. (R[0]. 0 means 1D reference model)

    vector<vector<double>> R(1);
    for (size_t i=0;i<gridDepth1.size();++i){
        auto tmpr=CreateGrid(_RE-gridDepth2[i],_RE-gridDepth1[i],gridInc[i],2);
        if (!R[0].empty()) R[0].pop_back();
        R[0].insert(R[0].end(),tmpr.rbegin(),tmpr.rend());
    }


    // Fix round-off-errors:
    // adding the exact double values in A. special depths and B. modefied 1D model to R[0].
    set<double> depthToCorrect(specialDepths.begin(),specialDepths.end());
    for (const auto &item:Deviation) {
        depthToCorrect.insert(item[0]);
        depthToCorrect.insert(item[1]);
    }
    vector<double> tmpArray;
    swap(R[0],tmpArray);
    tmpArray[0]=_RE;tmpArray.back()=0;
    auto it=depthToCorrect.begin();
    for (int i=0;i<(int)tmpArray.size();++i) {
        if (it==depthToCorrect.end())
            R[0].push_back(tmpArray[i]);
        else if (tmpArray[i]==_RE-*it) {
            R[0].push_back(tmpArray[i]);
            ++it;
        }
        else if (tmpArray[i]>_RE-*it) {
            R[0].push_back(tmpArray[i]);
        }
        else {
            R[0].push_back(_RE-*it);
            ++it;
            --i;
        }
    }
    while (it!=depthToCorrect.end()){
        R[0].push_back(_RE-*it);
        ++it;
    }

    // Find the bounds of input polygons.
    vector<vector<double>> RegionBounds{{-numeric_limits<double>::max(),numeric_limits<double>::max(),
        -numeric_limits<double>::max(),numeric_limits<double>::max()}};
    // the 1D reference bounds is as large as possible.

    for (size_t i=0;i<regionPolygonsTheta.size();++i){
        double Xmin=numeric_limits<double>::max(),Xmax=-Xmin,Ymin=Xmin,Ymax=-Ymin;
        for (size_t j=0;j<regionPolygonsTheta[i].size();++j){
            size_t k=(j+1)%regionPolygonsTheta[i].size();
            double theta1=regionPolygonsTheta[i][j],theta2=regionPolygonsTheta[i][k];
            double radius1=_RE-regionPolygonsDepth[i][j],radius2=_RE-regionPolygonsDepth[i][k];

            Xmin=min(Xmin,theta1);Xmax=max(Xmax,theta2);
            Ymin=min(Ymin,radius1);Ymax=max(Ymax,radius2);
        }
        RegionBounds.push_back({Xmin,Xmax,Ymin,Ymax});
    }


    // Find the closest layer value in R[0] to Ymin/Ymax of each polygon.
    vector<size_t> adjustedYmin{0},adjustedYmax{0};
    for (size_t i=0;i<regionPolygonsTheta.size();++i){
        adjustedYmin.push_back(findClosetLayer(R[0],RegionBounds[i+1][2]));
        adjustedYmax.push_back(findClosetLayer(R[0],RegionBounds[i+1][3]));
    }


    // Rectify input polygons. And derived the polygon layers from the layers of the 1D reference:
    vector<pair<double,double>> tmpRegion;
    vector<vector<pair<double,double>>> Regions{tmpRegion}; // place holder for Region[0], which is the 1D reference.

    for (size_t i=0;i<regionPolygonsTheta.size();++i){

        tmpRegion.clear();
        for (size_t j=0;j<regionPolygonsTheta[i].size();++j){

            // Find the fine enough rectify for this section.
            size_t k=(j+1)%regionPolygonsTheta[i].size();
            double radius1=_RE-regionPolygonsDepth[i][j],radius2=_RE-regionPolygonsDepth[i][k];
            double theta1=regionPolygonsTheta[i][j],theta2=regionPolygonsTheta[i][k];

            if (radius1==RegionBounds[i+1][2]) radius1=R[0][adjustedYmin[i+1]];
            if (radius1==RegionBounds[i+1][3]) radius1=R[0][adjustedYmax[i+1]];
            if (radius2==RegionBounds[i+1][2]) radius2=R[0][adjustedYmin[i+1]];
            if (radius2==RegionBounds[i+1][3]) radius2=R[0][adjustedYmax[i+1]];

            double Tdist=theta2-theta1,Rdist=radius2-radius1;

            size_t NPTS=2;
            double dL=_RE,dR=Rdist,dT=Tdist;
            while (dL>RectifyLimit){
                NPTS*=2;
                dR=Rdist/(NPTS-1);
                dT=Tdist/(NPTS-1);
                dL=LocDist(theta1,0,radius1,theta1+dT,0,radius1+dR);
            }

            // Add rectified segments to this polygon.
            for (size_t k=0;k+1<NPTS;++k)
                tmpRegion.push_back(make_pair(theta1+k*dT,radius1+k*dR));
        }

        // Add this rectified polygon to region array.
        Regions.push_back(tmpRegion);

        RegionN[i]=(int)tmpRegion.size();
        RegionsTheta[i]=(double *)malloc(tmpRegion.size()*sizeof(double));
        RegionsRadius[i]=(double *)malloc(tmpRegion.size()*sizeof(double));
        for (int j=0;j<RegionN[i];++j){
            RegionsTheta[i][j]=tmpRegion[j].first;
            RegionsRadius[i][j]=tmpRegion[j].second;
        }

    }

    // adjust bounds to the values in R[0].
    for (size_t i=0;i<regionPolygonsTheta.size();++i){
        RegionBounds[i+1][2]=R[0][adjustedYmin[i+1]];
        RegionBounds[i+1][3]=R[0][adjustedYmax[i+1]];
    }

    // derive layers for this polygon.
    for (size_t i=0;i<regionPolygonsTheta.size();++i)
        R.push_back(vector<double> (R[0].begin()+adjustedYmax[i+1],R[0].begin()+adjustedYmin[i+1]+1));

    // properties for these polygon.
    vector<double> dVp{1},dVs{1},dRho{1}; // Region 0 has dVp=1 ,...
    for (size_t i=0;i<regionPolygonsTheta.size();++i) {
        dVp.push_back(1.0+regionProperties[i][0]/100);
        dVs.push_back(1.0+regionProperties[i][1]/100);
        dRho.push_back(1.0+regionProperties[i][2]/100);
    }

    // derive properties layers for all regions (include 1D reference).
    vector<vector<double>> Vp(R.size(),vector<double> ()),Vs=Vp,Rho=Vp;
    for (size_t i=0;i<R.size();++i) {
        for (const auto &item:R[i]) {
            auto ans=MakeRef(_RE-item,Deviation);
            Vp[i].push_back(dVp[i]*ans[0]);
            Vs[i].push_back(dVs[i]*ans[1]);
            Rho[i].push_back(dRho[i]*ans[2]);
        }
    }

    // Create initial rays.
    for (size_t i=0;i<initRaySteps.size();++i){

        // Source in any polygons?
        size_t rid=0;
        for (size_t i=1;i<Regions.size();++i)
            if (PointInPolygon(Regions[i],make_pair(initRayTheta[i],_RE-initRayDepth[i]),1,RegionBounds[i])) {rid=i;break;}

        // Calculate ray parameter.
        auto ans=MakeRef(initRayDepth[i],Deviation);
        double v=(initRayComp[i]==0?ans[0]*dVp[rid]:ans[1]*dVs[rid]);
        double rayp=M_PI/180*(_RE-initRayDepth[i])*sin(fabs(initRayTakeoff[i])/180*M_PI)/v;

        // Push this ray into "RayHeads" for future processing.
        RayHeads.push_back(Ray(initRayComp[i]==0,fabs(initRayTakeoff[i])>=90,initRayTakeoff[i]<0,
                    (initRayComp[i]==0?"P":(initRayComp[i]==1?"SV":"SH")),
                    (int)rid,initRaySteps[i],initRayColor[i],
                    initRayTheta[i],_RE-initRayDepth[i],0,0,rayp,initRayTakeoff[i]));
    }

    atomic<size_t> Cnt;
    Cnt.store(RayHeads.size());
    atomic<int> Estimation;
    Estimation.store((int)potentialSize);
    RayHeads.resize(potentialSize);

    // Start ray tracing. (Finally!)
    //
    // Process each "Ray" leg in "RayHeads".
    // For future legs generated by reflction/refraction, create new "Ray" and assign it to the proper position in "RayHeads" vector.
    vector<thread> allThread(RayHeads.capacity());

    size_t Doing=0,Done=0;
    atomic<size_t> Running;
    Running.store(0);
    while (Doing!=Cnt.load() || Running.load()!=0) {
        *Observer=(int)Doing-(int)nThread;

        if (Running.load()<nThread && Doing<Cnt.load()) {
            Running.fetch_add(1);
            allThread[Doing]=thread(
                followThisRay,Doing, std::ref(Cnt), std::ref(Estimation), std::ref(Running),
                ReachSurfaces,ReachSurfacesSize,RayInfo,RayInfoSize, RaysTheta, RaysN, RaysRadius,
                std::ref(RayHeads), branches, std::cref(specialDepths),
                std::cref(R),std::cref(Vp),std::cref(Vs),std::cref(Rho),
                std::cref(Regions),std::cref(RegionBounds),std::cref(dVp),std::cref(dVs),std::cref(dRho),
                std::cref(DebugInfo),std::cref(TS),std::cref(TD),std::cref(RS),std::cref(RD),std::cref(StopAtSurface));
            if (Doing>0 && Doing%10000==0) {
                for (size_t i=Done;i<Doing-nThread;++i)
                    allThread[i].join();
                Done=Doing-nThread;
            }
            ++Doing;
        }
        else usleep(1000);

    } // End of ray tracing.

    for (size_t i=Done;i<Doing;++i)
        allThread[i].join();

//     cout << Cnt.load() << "/" << RayHeads.capacity() << "/" << Estimation.load() << endl;
}

// generating rays born from RayHeads[i]
void followThisRay(
    size_t i, atomic<size_t> &Cnt, atomic<int> &Estimation, atomic<size_t> &Running,
    char **ReachSurfaces, int *ReachSurfacesSize, char **RayInfo, int *RayInfoSize,
    double **RaysTheta, int *RaysN, double **RaysRadius,
    vector<Ray> &RayHeads, int branches, const vector<double> &specialDepths,
    const vector<vector<double>> &R, const vector<vector<double>> &Vp,
    const vector<vector<double>> &Vs,const vector<vector<double>> &Rho,
    const vector<vector<pair<double,double>>> &Regions, const vector<vector<double>> &RegionBounds,
    const vector<double> &dVp, const vector<double> &dVs,const vector<double> &dRho,
    const bool &DebugInfo,const bool &TS,const bool &TD,const bool &RS,const bool &RD, const bool &StopAtSurface){

    if (RayHeads[i].RemainingLegs==0) {
        Running.fetch_sub(1);
        return;
    }


    // Locate the begining and ending depths for the next leg.

    /// ... among special depths.

    //// Which special depth is cloest to ray head depth?
    double RayHeadDepth=_RE-RayHeads[i].Pr;
    size_t Cloest=findClosetDepth(specialDepths,RayHeadDepth);

    //// Next depth should be the cloest special depth at the correct side (ray is going up/down).
    //// Is the ray going up or down? Is the ray already at the cloest special depth? If yes, adjust the next depth.
    double NextDepth=specialDepths[Cloest];
    if (RayHeads[i].GoUp && (specialDepths[Cloest]>RayHeadDepth || RayHeadDepth==specialDepths[Cloest]))
        NextDepth=specialDepths[Cloest-1];
    else if (!RayHeads[i].GoUp && (specialDepths[Cloest]<RayHeadDepth || specialDepths[Cloest]==RayHeadDepth))
        NextDepth=specialDepths[Cloest+1];

    double Top=min(RayHeadDepth,NextDepth),Bot=max(RayHeadDepth,NextDepth);

    /// ... among current 2D "Regions" vertical limits.
    int CurRegion=RayHeads[i].InRegion;
    Top=max(Top,_RE-RegionBounds[CurRegion][3]);
    Bot=min(Bot,_RE-RegionBounds[CurRegion][2]);

    // Print some debug info.
    if (DebugInfo) {
        RayHeads[i].Debug+=to_string(1+i)+" --> ";
        cout << '\n' << "----------------------" ;
        cout << '\n' << "Calculating    : " << RayHeads[i].Debug;
        cout << "\nStart in region       : " << CurRegion;
        printf ("\nStart Location        : %.15lf deg, %.15lf km\n", RayHeads[i].Pt, _RE-RayHeads[i].Pr);
        cout << "Will go as            : " << (RayHeads[i].IsP?"P, ":"S, ") << (RayHeads[i].GoUp?"Up, ":"Down, ")
            << (RayHeads[i].GoLeft?"Left":"Right") << endl;
        printf ("Ray tracing start, end: %.16lf --> %.16lf km with rayp: %.16lf sec/deg\n",Top,Bot,RayHeads[i].RayP);
        printf ("Search region bounds  : %.16lf ~ %.16lf\n", _RE-R[CurRegion][0],_RE-R[CurRegion].back());
        printf ("\nStart ray tracing ...\n\n");
        cout << flush;
    }


    // Use ray-tracing code "RayPath".
    size_t lastRadiusIndex;
    vector<double> degree;
    const auto &v=(RayHeads[i].IsP?Vp:Vs);
    auto ans=RayPath(R[CurRegion],v[CurRegion],RayHeads[i].RayP,Top,Bot,degree,lastRadiusIndex,_TURNINGANGLE);


    // If the new leg is trivia, no further operation needed.
    size_t RayLength=degree.size();
    if (RayLength==1) {
        RayHeads[i].RemainingLegs=0;
        Running.fetch_sub(1);
        return;
    }


    // This should never happen.
    // If the new leg is a reflection of down-going S to up-going P, and also the new leg turns, also mark it as invalid.
    //     int PrevID=RayHeads[i].Prev;
    //     if (PrevID!=-1 && !RayHeads[PrevID].GoUp && !RayHeads[PrevID].IsP && RayHeads[i].GoUp && RayHeads[i].IsP && ans.second) {
    //         RayHeads[i].RemainingLegs=0;
    //         Running.fetch_sub(1);
    //         return;
    //     }


    // Reverse the ray-tracing result if new leg is going upward.
    if (RayHeads[i].GoUp) {
        double totalDist=degree.back();
        for (auto &item:degree) item=totalDist-item;
        reverse(degree.begin(),degree.end());
    }


    // Create a projection from ray index to layer index.
    bool uP=RayHeads[i].GoUp;
    auto rIndex = [RayLength,lastRadiusIndex,uP](size_t j){
        if (uP) return (int)lastRadiusIndex-(int)j;
        else return (int)j+(int)lastRadiusIndex-(int)RayLength+1;
    };


    // Follow the new ray path to see if the new leg enters another region.
    int RayEnd=-1,NextRegion=-1,M=(RayHeads[i].GoLeft?-1:1);

    for (size_t j=0;j<degree.size();++j){

        pair<double,double> p={RayHeads[i].Pt+M*degree[j],R[CurRegion][rIndex(j)]}; // point on the newly calculated ray.

        if (CurRegion!=0){ // starts in some 2D polygon ...

            if (PointInPolygon(Regions[CurRegion],p,-1,RegionBounds[CurRegion])) continue; // ... and this point stays in that polygon.
            else { // ... but this point enters another polygon.

                RayEnd=(int)j;

                // which region is the new leg entering?
                for (size_t k=1;k<Regions.size();++k){
                    if ((int)k==CurRegion) continue;
                    if (PointInPolygon(Regions[k],p,1,RegionBounds[k])) {
                        NextRegion=(int)k;
                        break;
                    }
                }
                if (NextRegion==-1) NextRegion=0; // if can't find next 2D polygons, it must had return to the 1D reference region.
                break;
            }
        }
        else { // New leg starts in 1D reference region. Search for the region it enters.
            for (size_t k=1;k<Regions.size();++k){
                if (PointInPolygon(Regions[k],p,-1,RegionBounds[k])) { // If ray enters another region.
                    RayEnd=(int)j;
                    NextRegion=(int)k;
                    break;
                }
            }
            if (RayEnd!=-1) break; // If ray enters another region.
            else NextRegion=0;
        }
    }


    // Print some debug info.
    if (DebugInfo) {
        cout << "Next refraction region is    : " << NextRegion << endl;
    }


    // Prepare reflection/refraction flags. (notice "rs" [r]eflection to [s]ame wave type is always possible)
    bool ts=TS,td=(TD && RayHeads[i].Comp!="SH"),rd=(RD && RayHeads[i].Comp!="SH");


    // Locate the end of new leg, which is needed to calculate incident angle, coefficients, next ray parameter, etc.
    // (if interface is not horizontal("TiltAngle"), ray parameter will change.)
    //
    // Decision made: If the last line segment of the new leg crosses interface(at "JuncPt/JuncPr"),
    // for reflection: the end point outside of new region ("NextP?_R") is the next ray starting point;
    // for refraction: the end point inside of the new region ("NextP?_T") is the next ray starting piont.
    double NextPt_R,NextPr_R,NextPt_T,NextPr_T,JuncPt,JuncPr,TiltAngle,Rayp_td=-1,Rayp_ts=-1,Rayp_rd=-1,Rayp_rs=-1;

    pair<double,double> p2,q2; // Two end points of the last line segment of the new leg.
    // Notice, for normal rays hit the horizontal interface, one end point is on the interface.

    if (RayEnd!=-1){ // If ray ends pre-maturely (last line segment crossing the interface).

        // For reflection, the future rays start from the last point in the current region (index: RayEnd-1).
        NextPt_R=RayHeads[i].Pt+M*degree[RayEnd-1];
        NextPr_R=R[CurRegion][rIndex(RayEnd-1)];


        // For transmission/refraction, the futuer rays start from the first point in the next region (index: RayEnd).
        NextPt_T=RayHeads[i].Pt+M*degree[RayEnd];
        NextPr_T=R[CurRegion][rIndex(RayEnd)];


        // Re-calculate travel distance and travel time till the last point in the current region.
        ans.first.first=ans.first.second=0;
        for (int j=0;j<RayEnd-1;++j){
            double dist=sqrt( pow(R[CurRegion][rIndex(j)],2) + pow(R[CurRegion][rIndex(j+1)],2)
                    -2*R[CurRegion][rIndex(j)]*R[CurRegion][rIndex(j+1)]*cos(M_PI/180*(degree[j+1]-degree[j])) );
            ans.first.second+=dist;
            ans.first.first+=dist/v[CurRegion][rIndex(j+1)];
        }


        // Find the junction between the last line segment (index: RayEnd-1 ~ RayEnd) and polygon boundary segment (index: L1 ~ L2).
        size_t L1=0,L2=1,SearchRegion=(NextRegion==0?CurRegion:NextRegion);
        p2={NextPt_R,NextPr_R};
        q2={NextPt_T,NextPr_T};
        pair<bool,pair<double,double>> res;


        for (L1=0;L1<Regions[SearchRegion].size();++L1){ // Search around the polygon.
            L2=(L1+1)%Regions[SearchRegion].size();
            res=SegmentJunction(Regions[SearchRegion][L1],Regions[SearchRegion][L2],p2,q2);
            if (res.first) break;
        }
        if (L1==Regions[SearchRegion].size()) {
            throw runtime_error("!!!!!!!!!!! Can't find junction! Bugs here !!!!!!!!!!!");
        }

        // Find the junction point between ray and polygon boundary.
        JuncPt=res.second.first;
        JuncPr=res.second.second;

        // Print some debug info.
        if (DebugInfo) printf("Junction at       : %.15lf,%.15lf.\n",JuncPt,JuncPr);


        // Twick travel times and travel distance, compensate for the lost part.
        double dlx=(p2.first-JuncPt)*M_PI*JuncPr/180,dly=p2.second-JuncPr;
        double dl=sqrt(dlx*dlx+dly*dly);
        ans.first.second+=dl;
        ans.first.first+=dl/v[CurRegion][rIndex(RayEnd-1)]; // Use the velocit within current region to avoid possible "inf" travel time.


        // Get the geometry of the boundary.
        const pair<double,double> &p1=Regions[SearchRegion][L1],&q1=Regions[SearchRegion][L2];
        TiltAngle=180/M_PI*atan2(q1.second-p1.second,(q1.first-p1.first)*M_PI/180*JuncPr);

    }
    else { // If ray doesn't end pre-maturelly (stays in the same region and reflect/refract on horizontal intervals)
        // (one end point of the last line segment (index: RayEnd-1) is on the interface)

        RayEnd=(int)degree.size();
        NextRegion=CurRegion;

        // Get futuer rays starting point.
        NextPt_T=NextPt_R=RayHeads[i].Pt+M*degree[RayEnd-1];
        NextPr_T=NextPr_R=R[CurRegion][rIndex(RayEnd-1)];


        // Notice ray parameter doesn't change if reflection/refraction interface is horizontal.
        Rayp_td=Rayp_ts=Rayp_rd=Rayp_rs=RayHeads[i].RayP;


        // Get the last segment of the new leg.
        p2={RayHeads[i].Pt+M*degree[RayEnd-2],R[CurRegion][rIndex(RayEnd-2)]};
        q2={NextPt_T,NextPr_T};

        // Get the geometry of the boundary.
        TiltAngle=0;
        JuncPt=NextPt_T;
        JuncPr=NextPr_T;

    } // End of dealing with rays entering another region.


    // Get the geometry of the last section. (Ray direction: "Rayd" [-180 ~ 180])
    double Rayd=180/M_PI*atan2(q2.second-p2.second,(q2.first-p2.first)*M_PI/180*JuncPr);


    // Calculate incident angle. (the acute angle between ray direction and the normal of interface)
    double Rayd_Hor=Lon2360(Rayd-TiltAngle),Incident=fabs(Lon2180(Rayd_Hor));
    Incident=(Incident>90?Incident-90:90-Incident);


    // Print some debug info.
    if (DebugInfo) {
        printf("Incident angle    : %.15lf deg\n",Incident);
        printf("Ray direction     : %.15lf deg\n",Rayd);
        printf("Interface tilt    : %.15lf deg\n",TiltAngle);
    }


    // Prepare to calculate reflection/refractoin(transmission) coefficients.
    string Mode,Polarity=(RayHeads[i].Comp=="SH"?"SH":"PSV");
    if (NextPr_R==_RE) Mode="SA"; // At the surface.
    else if (NextPr_R==3480) Mode=(RayHeads[i].GoUp?"LS":"SL"); // At the CMB.
    else if (NextPr_R==1221.5) Mode=(RayHeads[i].GoUp?"SL":"LS"); // At the ICB.
    else if (1221.5<NextPr_R && NextPr_R<3480) Mode="LL"; // Within outer core.
    else Mode="SS";

    double rho1,vp1,vs1,rho2,vp2,vs2,c1,c2;;

    if (CurRegion!=NextRegion) { // if ray enters a different region.
        rho1=Rho[CurRegion][rIndex(RayEnd-1)];
        rho2=Rho[CurRegion][rIndex(RayEnd)]/dRho[CurRegion]*dRho[NextRegion];
        vp1=Vp[CurRegion][rIndex(RayEnd-1)];
        vp2=Vp[CurRegion][rIndex(RayEnd)]/dVp[CurRegion]*dVp[NextRegion];
        vs1=Vs[CurRegion][rIndex(RayEnd-1)];
        vs2=Vs[CurRegion][rIndex(RayEnd)]/dVs[CurRegion]*dVs[NextRegion];
    }
    else { // if ray stays in the same region. ray ends normally.
        int si=RayEnd;
        if (RayHeads[i].GoUp) --si;

        rho1=Rho[CurRegion][rIndex(si-1)];
        rho2=Rho[CurRegion][rIndex(si)];
        vp1=Vp[CurRegion][rIndex(si-1)];
        vp2=Vp[CurRegion][rIndex(si)];
        vs1=Vs[CurRegion][rIndex(si-1)];
        vs2=Vs[CurRegion][rIndex(si)];
    }
    if (vs1<0.01) Mode[0]='L';
    if (vs2<0.01 && Mode[1]=='S') Mode[1]='L';

    // Print some debug info.
    if (DebugInfo) printf("rho1/vp1/vs1      : %.15lf g/cm3, %.15lf km/sec, %.15lf km/sec\n",rho1,vp1,vs1);
    if (DebugInfo) printf("rho2/vp2/vs2      : %.15lf g/cm3, %.15lf km/sec, %.15lf km/sec\n",rho2,vp2,vs2);

    complex<double> R_PP,R_PS,R_SP,R_SS,T_PP,T_PS,T_SP,T_SS;
    R_PP=R_PS=R_SP=R_SS=T_PP=T_PS=T_SP=T_SS=0;

    /// A. Refractions/Transmissions to the same wave type.

    //// Coefficients. (T_PP,T_SS)
    auto Coef=PlaneWaveCoefficients(rho1,vp1,vs1,rho2,vp2,vs2,Incident,Polarity,Mode);
    if (Mode=="SS") {
        if (RayHeads[i].Comp=="SH") T_SS=Coef[1];
        else {T_PP=Coef[4];T_SS=Coef[7];}
    }
    if (Mode=="SL" && RayHeads[i].Comp=="P") T_PP=Coef[4];
    if (Mode=="LS" && RayHeads[i].Comp=="P") T_PP=Coef[1];
    if (Mode=="LL" && RayHeads[i].Comp=="P") T_PP=Coef[1];

    //// take-off angles.
    if (RayHeads[i].IsP) {c1=vp1;c2=vp2;}
    else {c1=vs1;c2=vs2;}

    //// difference between incident angle and takeoff angle.
    double Takeoff_ts=asin(sin(Incident*M_PI/180)*c2/c1)*180/M_PI-Incident;
    ts&=!std::isnan(Takeoff_ts);

    //// take 2D structure shape into consideration.
    if ((0<Rayd_Hor && Rayd_Hor<=90) || (180<Rayd_Hor && Rayd_Hor<=270)) Takeoff_ts=Lon2180(Rayd_Hor-Takeoff_ts+TiltAngle+90);
    else Takeoff_ts=Lon2180(Rayd_Hor+Takeoff_ts+TiltAngle+90);

    //// new ray paramter.
    if (CurRegion!=NextRegion) Rayp_ts=M_PI/180*NextPr_T*sin(fabs(Takeoff_ts)*M_PI/180)/c2;
    ts&=!std::isnan(Rayp_ts);


    /// B. Refractions/Transmissions to different wave type.

    //// Coefficients. (T_PS,T_SP)
    if (Mode=="SS" && RayHeads[i].Comp!="SH") {T_PS=Coef[1];T_SP=Coef[6];}
    if (Mode=="SL" && RayHeads[i].Comp=="SV") T_SP=Coef[5];
    if (Mode=="LS" && RayHeads[i].Comp=="P") T_PS=Coef[2];

    //// take-off angles.
    if (RayHeads[i].IsP) {c1=vp1;c2=vs2;}
    else {c1=vs1;c2=vp2;}

    //// difference between incident angle and takeoff angle.
    double Takeoff_td=asin(sin(Incident*M_PI/180)*c2/c1)*180/M_PI-Incident;
    td&=!std::isnan(Takeoff_td);

    //// take 2D structure shape into consideration.
    if ((0<Rayd_Hor && Rayd_Hor<=90) || (180<Rayd_Hor && Rayd_Hor<=270)) Takeoff_td=Lon2180(Rayd_Hor-Takeoff_td+TiltAngle+90);
    else Takeoff_td=Lon2180(Rayd_Hor+Takeoff_td+TiltAngle+90);

    //// new ray paramter.
    if (CurRegion!=NextRegion) Rayp_td=M_PI/180*NextPr_T*sin(fabs(Takeoff_td)*M_PI/180)/c2;
    td&=!std::isnan(Rayp_td);


    /// C. Reflection to a different wave type.

    //// Coefficients. (R_PS,R_SP)
    if (Mode=="SS" && RayHeads[i].Comp!="SH") {R_PS=Coef[1];R_SP=Coef[2];}
    if (Mode=="SL" && RayHeads[i].Comp!="SH") {R_PS=Coef[1];R_SP=Coef[2];}
    if (Mode=="SA" && RayHeads[i].Comp!="SH") {R_PS=Coef[1];R_SP=Coef[2];}

    //// take-off angles.
    c1=vs1;c2=vp1;
    if (RayHeads[i].IsP) swap(c1,c2);

    //// difference between incident angle and takeoff angle.
    double Takeoff_rd=asin(sin(Incident*M_PI/180)*c2/c1)*180/M_PI-Incident;
    rd&=!std::isnan(Takeoff_rd);

    //// take 2D structure shape into consideration.
    double x=Lon2360(-Rayd_Hor);
    if ((0<x && x<=90) || (180<x && x<=270)) Takeoff_rd=Lon2180(x-Takeoff_rd+TiltAngle+90);
    else Takeoff_rd=Lon2180(x+Takeoff_rd+TiltAngle+90);

    //// new ray paramter.
    if (CurRegion!=NextRegion) Rayp_rd=M_PI/180*NextPr_R*sin(fabs(Takeoff_rd)*M_PI/180)/c2;
    rd&=!std::isnan(Rayp_rd);


    /// D. reflection to a same wave type.
    //// Coefficients. (R_PP,R_SS)
    if (Mode=="SS") {
        if (RayHeads[i].Comp=="SH") R_SS=Coef[0];
        else {R_PP=Coef[0];R_SS=Coef[3];}
    }
    if (Mode=="SL") {
        if (RayHeads[i].Comp=="SH") R_SS=1.0;
        else {R_PP=Coef[0];R_SS=Coef[3];}
    }
    if (Mode=="SA") {
        if (RayHeads[i].Comp=="SH") R_SS=1.0;
        else {R_PP=Coef[0];R_SS=Coef[3];}
    }
    if ((Mode=="LS" || Mode=="LL") && RayHeads[i].Comp=="P") R_PP=Coef[0];
    if (ans.second) R_SS=R_PP=1;

    //// new ray paramter.
    double Takeoff_rs=Lon2180(-Rayd_Hor+TiltAngle+90);
    if (CurRegion!=NextRegion) Rayp_rs=M_PI/180*NextPr_R*sin(fabs(Takeoff_rs)*M_PI/180)/c1;


    // Print some debug info.
    if (DebugInfo) {
        printf("\nTakeOff angle (TS): %.15lf deg\n",Takeoff_ts);
        printf("RayP          (TS): %.15lf\n",Rayp_ts);
        printf("TakeOff angle (TD): %.15lf deg\n",Takeoff_td);
        printf("RayP          (TD): %.15lf deg\n",Rayp_td);
        printf("TakeOff angle (RD): %.15lf deg\n",Takeoff_rd);
        printf("RayP          (RD): %.15lf deg\n",Rayp_rd);
        printf("TakeOff angle (RS): %.15lf deg\n",Takeoff_rs);
        printf("RayP          (RS): %.15lf deg\n\n",Rayp_rs);

        cout << "RayEnd at Index  :" << RayEnd-1 << " (inclusive) / " << RayLength << endl;
        printf ("RayEnd at (for reflection)   :%.15lf deg, %.15lf (inclusive) km\n",NextPt_R,_RE-NextPr_R);
        printf ("RayEnd at (for transmission) :%.15lf deg, %.15lf (inclusive) km\n\n",NextPt_T,_RE-NextPr_T);
        cout << endl;
    }

    // Update current RayHead.
    RayHeads[i].TravelTime=ans.first.first;
    RayHeads[i].TravelDist=ans.first.second;
    --RayHeads[i].RemainingLegs;
    RayHeads[i].Inc=Incident;


    // store ray paths.
    stringstream ss;
    ss << RayHeads[i].Color << " "
       << (RayHeads[i].IsP?"P ":"S ") << RayHeads[i].TravelTime << " sec. " << RayHeads[i].Inc << " IncDeg. "
       << RayHeads[i].Amp << " DispAmp. " << RayHeads[i].TravelDist << " km. ";
    string tmpstr=ss.str();
    RayInfoSize[i]=(int)tmpstr.size()+1;
    RayInfo[i]=(char *)malloc((tmpstr.size()+1)*sizeof(char));
    strcpy(RayInfo[i],tmpstr.c_str());

    RaysN[i]=RayEnd;
    RaysTheta[i]=(double *)malloc(RayEnd*sizeof(double));
    RaysRadius[i]=(double *)malloc(RayEnd*sizeof(double));
    for (int j=0;j<RayEnd;++j) {
        RaysTheta[i][j]=RayHeads[i].Pt+M*degree[j];
        RaysRadius[i][j]=R[CurRegion][rIndex(j)];
    }

    // If ray reaches surface, output info at the surface.
    if (NextPr_R==_RE) ++RayHeads[i].Surfacing;
    if (NextPr_R==_RE && (StopAtSurface==0 || RayHeads[i].Surfacing<2)) {

        // Accumulate the travel-time.
        int I=(int)i;
        double tt=0;
        vector<int> hh;
        while (I!=-1) {
            hh.push_back(I);
            tt+=RayHeads[I].TravelTime;
            I=RayHeads[I].Prev;
        }

        stringstream ss;
        ss << RayHeads[hh.back()].Takeoff << " " << RayHeads[i].RayP << " " << RayHeads[i].Inc << " " << NextPt_R << " "
            << tt << " " << RayHeads[i].Amp << " " << RayHeads[i].RemainingLegs << " ";
        for (auto rit=hh.rbegin();rit!=hh.rend();++rit)
            ss << (RayHeads[*rit].IsP?(RayHeads[*rit].GoUp?"p":"P"):(RayHeads[*rit].GoUp?"s":"S")) << ((*rit)==*hh.begin()?" ":"->");
        for (auto rit=hh.rbegin();rit!=hh.rend();++rit)
            ss << (1+*rit) << ((*rit)==*hh.begin()?"":"->");

        string tmpstr=ss.str();
        if (!tmpstr.empty()) {
            ReachSurfacesSize[i]=(int)tmpstr.size()+1;
            ReachSurfaces[i]=(char *)malloc((tmpstr.size()+1)*sizeof(char));
            strcpy(ReachSurfaces[i],tmpstr.c_str());
        }

        if (StopAtSurface==1) {
            int z=RayHeads[i].RemainingLegs;
            if (branches>1) z=(1-pow(branches,RayHeads[i].RemainingLegs))/(1-branches);
            Estimation.fetch_sub(branches*z);
            Running.fetch_sub(1);
            return;
        }
    }

    if (RayHeads[i].RemainingLegs==0) {
        Running.fetch_sub(1);
        return;
    }


    // Add rules of: (t)ransmission/refrection and (r)eflection to (s)ame or (d)ifferent way type.
    // Notice reflection with the same wave type is always allowed. ("rs" is always possible)

    /// if ray going down and turns.
    if (!RayHeads[i].GoUp && ans.second) ts=td=rd=false;

    /// if ray ends at the surface.
    if (NextPr_R==_RE) ts=td=false;

    /// if ray goes down to CMB, no transmission S; if ray goes up to ICB, not transmission S.
    if ((!RayHeads[i].GoUp && NextPr_T==3480) || (RayHeads[i].GoUp && NextPr_T==1221.5)) {
        ts&=RayHeads[i].IsP;
        td&=(!RayHeads[i].IsP);
    }

    /// if ray goes down to ICB as P, no reflection S.
    if (!RayHeads[i].GoUp && RayHeads[i].IsP && NextPr_R==1221.5) rd=false;

    /// if ray goes up to CMB as P, no reflection S.
    if (RayHeads[i].GoUp && RayHeads[i].IsP && NextPr_R==3480) rd=false;

    // Add new ray heads to "RayHeads" according to the rules ans reflection/refraction angle calculation results.

    if (ts) {
        Ray newRay=RayHeads[i];
        newRay.Prev=(int)i;
        newRay.Pt=NextPt_T;
        newRay.Pr=NextPr_T;
        newRay.RayP=Rayp_ts;
        newRay.GoUp=(fabs(Takeoff_ts)>90);
        newRay.GoLeft=(Takeoff_ts<0);
        newRay.InRegion=NextRegion;
        double sign1=(T_PP.imag()==0?(T_PP.real()<0?-1:1):1);
        double sign2=(T_SS.imag()==0?(T_SS.real()<0?-1:1):1);
        newRay.Amp*=(newRay.IsP?(sign1*abs(T_PP)):(sign2*abs(T_SS)));
        RayHeads[Cnt.fetch_add(1)]=newRay;
    }

    if (td) {
        Ray newRay=RayHeads[i];
        newRay.IsP=!newRay.IsP;
        newRay.Prev=(int)i;
        newRay.Pt=NextPt_T;
        newRay.Pr=NextPr_T;
        newRay.RayP=Rayp_td;
        newRay.GoUp=(fabs(Takeoff_td)>90);
        newRay.GoLeft=(Takeoff_td<0);
        newRay.InRegion=NextRegion;
        double sign1=(T_PS.imag()==0?(T_PS.real()<0?-1:1):1);
        double sign2=(T_SP.imag()==0?(T_SP.real()<0?-1:1):1);
        newRay.Amp*=(newRay.IsP?(sign1*abs(T_PS)):(sign2*abs(T_SP)));
        RayHeads[Cnt.fetch_add(1)]=newRay;
    }

    if (rd) {
        Ray newRay=RayHeads[i];
        newRay.IsP=!newRay.IsP;
        newRay.Prev=(int)i;
        newRay.Pt=NextPt_R;
        newRay.Pr=NextPr_R;
        newRay.RayP=Rayp_rd;
        newRay.GoUp=(fabs(Takeoff_rd)>90);
        newRay.GoLeft=(Takeoff_rd<0);
        double sign1=(R_PS.imag()==0?(R_PS.real()<0?-1:1):1);
        double sign2=(R_SP.imag()==0?(R_SP.real()<0?-1:1):1);
        newRay.Amp*=(newRay.IsP?(sign1*abs(R_PS)):(sign2*abs(R_SP)));
        RayHeads[Cnt.fetch_add(1)]=newRay;
    }

    int y=(TS && !ts)+(TD && !td)+(RD && !rd);
    int z=RayHeads[i].RemainingLegs;
    if (branches>1) z=(1-pow(branches,RayHeads[i].RemainingLegs))/(1-branches);
    Estimation.fetch_sub(y*z);

    // rs is always possible.
    if (RS) {
        Ray newRay=RayHeads[i];
        newRay.Prev=(int)i;
        newRay.Pt=NextPt_R;
        newRay.Pr=NextPr_R;
        newRay.RayP=Rayp_rs;
        newRay.GoUp=(fabs(Takeoff_rs)>90);
        newRay.GoLeft=(Takeoff_rs<0);
        double sign1=(R_PP.imag()==0?(R_PP.real()<0?-1:1):1);
        double sign2=(R_SS.imag()==0?(R_SS.real()<0?-1:1):1);
        newRay.Amp*=(newRay.IsP?(sign1*abs(R_PP)):(sign2*abs(R_SS)));
        RayHeads[Cnt.fetch_add(1)]=newRay;
    }

    Running.fetch_sub(1);
    return;
}

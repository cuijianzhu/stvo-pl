#include <stereoFrame.h>
#include <future>

#include <mrpt/utils/CTicTac.h>

struct compare_descriptor_by_NN_dist
{
    inline bool operator()(const vector<DMatch>& a, const vector<DMatch>& b){
        return ( a[0].distance < b[0].distance );
    }
};

struct compare_descriptor_by_NN12_dist
{
    inline bool operator()(const vector<DMatch>& a, const vector<DMatch>& b){
        return ( a[1].distance - a[0].distance > b[1].distance-b[0].distance );
    }
};

struct compare_descriptor_by_NN12_ratio
{
    inline bool operator()(const vector<DMatch>& a, const vector<DMatch>& b){
        return ( a[1].distance / a[0].distance > b[1].distance / b[0].distance );
    }
};

struct sort_descriptor_by_queryIdx
{
    inline bool operator()(const vector<DMatch>& a, const vector<DMatch>& b){
        return ( a[0].queryIdx < b[0].queryIdx );
    }
};

struct sort_descriptor_by_trainIdx
{
    inline bool operator()(const vector<DMatch>& a, const vector<DMatch>& b){
        return ( a[0].trainIdx < b[0].trainIdx );
    }
};


StereoFrame::StereoFrame(){}

StereoFrame::StereoFrame(const Mat &img_l_, const Mat &img_r_ , const int idx_, PinholeStereoCamera *cam_) :
    img_l(img_l_), img_r(img_r_), frame_idx(idx_), cam(cam_) {}

StereoFrame::StereoFrame(const Mat &img_l_, const Mat &img_r_ , const int idx_, PinholeStereoCamera *cam_, Matrix4d DT_ini) :
    img_l(img_l_), img_r(img_r_), frame_idx(idx_), cam(cam_), DT(DT_ini) {}

StereoFrame::~StereoFrame(){}

void StereoFrame::extractStereoFeatures()
{

    // Feature detection and description
    vector<KeyPoint> points_l, points_r;
    vector<KeyLine>  lines_l, lines_r;

    mrpt::utils::CTicTac clk;
    double s1, s2, s3, s4, s5;

    clk.Tic();

    if( Config::lrInParallel() )
    {
        auto detect_l = async(launch::async, &StereoFrame::detectFeatures, this, img_l, ref(points_l), ref(pdesc_l), ref(lines_l), ref(ldesc_l) );
        auto detect_r = async(launch::async, &StereoFrame::detectFeatures, this, img_r, ref(points_r), ref(pdesc_r), ref(lines_r), ref(ldesc_r) );
        detect_l.wait();
        detect_r.wait();
    }
    else
    {
        detectFeatures(img_l,points_l,pdesc_l,lines_l,ldesc_l);
        detectFeatures(img_r,points_r,pdesc_r,lines_r,ldesc_r);
    }

    s1 = 1000.0 * clk.Tac(); clk.Tic();

    // Points stereo matching
    if( Config::hasPoints() && !(points_l.size()==0) && !(points_r.size()==0) )
    {
        BFMatcher* bfm = new BFMatcher( NORM_HAMMING, false );
        vector<vector<DMatch>> pmatches_lr, pmatches_rl, pmatches_lr_;
        Mat pdesc_l_;
        stereo_pt.clear();
        #pragma message("TODO: two different functions if we employ or not best LR and RL match")
        // LR and RL matches
        if( Config::lrInParallel() )
        {
            auto match_l = async( launch::async, &StereoFrame::matchPointFeatures, this, bfm, pdesc_l, pdesc_r, ref(pmatches_lr) );
            auto match_r = async( launch::async, &StereoFrame::matchPointFeatures, this, bfm, pdesc_r, pdesc_l, ref(pmatches_rl) );
            match_l.wait();
            match_r.wait();
        }
        else
        {
            bfm->knnMatch( pdesc_l, pdesc_r, pmatches_lr, 2);
            bfm->knnMatch( pdesc_r, pdesc_l, pmatches_rl, 2);
        }

        s2 = 1000.0 * clk.Tac(); clk.Tic();

        // ---------------------------------------------------------------------
        // sort matches by the distance between the best and second best matches
        #pragma message("TODO: try robust standard deviation (MAD)")
        double nn_dist_th, nn12_dist_th;
        pointDescriptorMAD(pmatches_lr,nn_dist_th, nn12_dist_th);
        nn_dist_th    = nn_dist_th   * Config::descThP();
        nn12_dist_th  = nn12_dist_th * Config::descThP();
        // ---------------------------------------------------------------------

        s3 = 1000.0 * clk.Tac(); clk.Tic();

        // resort according to the queryIdx
        sort( pmatches_lr.begin(), pmatches_lr.end(), sort_descriptor_by_queryIdx() );
        sort( pmatches_rl.begin(), pmatches_rl.end(), sort_descriptor_by_queryIdx() );

        s4 = 1000.0 * clk.Tac(); clk.Tic();

        // bucle around pmatches
        for( int i = 0; i < pmatches_lr.size(); i++ )
        {
            // check if they are mutual best matches
            int lr_qdx = pmatches_lr[i][0].queryIdx;
            int lr_tdx = pmatches_lr[i][0].trainIdx;
            //int rl_qdx = pmatches_rl[lr_tdx][0].queryIdx;
            int rl_tdx = pmatches_rl[lr_tdx][0].trainIdx;
            // check if they are mutual best matches and the minimum distance
            double dist_nn = pmatches_lr[i][0].distance;
            double dist_12 = pmatches_lr[i][1].distance - pmatches_lr[i][0].distance;
            if( lr_qdx == rl_tdx  && dist_12 > nn12_dist_th && dist_nn < nn_dist_th )
            {
                // check stereo epipolar constraint
                if( fabsf( points_l[lr_qdx].pt.y-points_r[lr_tdx].pt.y) <= Config::maxDistEpip() )
                {
                    // check minimal disparity
                    double disp_ = points_l[lr_qdx].pt.x - points_r[lr_tdx].pt.x;
                    if( disp_ >= Config::minDisp() ){
                        pdesc_l_.push_back( pdesc_l.row(lr_qdx) );
                        //pdesc_r_.push_back( pdesc_r.row(lr_qdx) );
                        PointFeature* point_;
                        Vector2d pl_; pl_ << points_l[lr_qdx].pt.x, points_l[lr_qdx].pt.y;
                        Vector3d P_;  P_ = cam->backProjection( pl_(0), pl_(1), disp_);
                        stereo_pt.push_back( new PointFeature(pl_,disp_,P_) );
                    }
                }
            }
        }
        pdesc_l_.copyTo(pdesc_l);

        s5 = 1000.0 * clk.Tac(); clk.Tic();

    }

    clk.Tic();

    // Line segments stereo matching
    if( Config::hasLines() && !lines_l.empty() && !lines_r.empty() )
    {
        stereo_ls.clear();
        Ptr<BinaryDescriptorMatcher> bdm = BinaryDescriptorMatcher::createBinaryDescriptorMatcher();
        vector<vector<DMatch>> lmatches_lr, lmatches_rl;
        Mat ldesc_l_;
        #pragma message("TODO: two different functions if we employ or not best LR and RL match")
        // LR and RL matches
        if( Config::lrInParallel() )
        {
            auto match_l = async( launch::async, &StereoFrame::matchLineFeatures, this, bdm, ldesc_l, ldesc_r, ref(lmatches_lr) );
            auto match_r = async( launch::async, &StereoFrame::matchLineFeatures, this, bdm, ldesc_r, ldesc_l, ref(lmatches_rl) );
            match_l.wait();
            match_r.wait();
        }
        else
        {
            bdm->knnMatch( ldesc_l,ldesc_r, lmatches_lr, 2);
            bdm->knnMatch( ldesc_r,ldesc_l, lmatches_rl, 2);
        }

        s2 += 1000.0 * clk.Tac(); clk.Tic();

        // ---------------------------------------------------------------------
        #pragma message("TODO: try MAD deviation")
        #pragma message("TODO: try filtering lines with segment's length")
        // sort matches by the distance between the best and second best matches
        double nn_dist_th, nn12_dist_th;
        lineDescriptorMAD( lmatches_lr, nn_dist_th, nn12_dist_th);
        nn_dist_th    = nn_dist_th   * Config::descThL();
        nn12_dist_th  = nn12_dist_th * Config::descThL();
        // ---------------------------------------------------------------------

        s3 += 1000.0 * clk.Tac(); clk.Tic();

        // bucle around pmatches
        sort( lmatches_lr.begin(), lmatches_lr.end(), sort_descriptor_by_queryIdx() );
        sort( lmatches_rl.begin(), lmatches_rl.end(), sort_descriptor_by_queryIdx() );

        s4 += 1000.0 * clk.Tac(); clk.Tic();

        int n_matches = min(lmatches_lr.size(),lmatches_rl.size());
        for( int i = 0; i < n_matches; i++ )
        {
            // check if they are mutual best matches
            int lr_qdx = lmatches_lr[i][0].queryIdx;
            int lr_tdx = lmatches_lr[i][0].trainIdx;
            int rl_tdx = lmatches_rl[lr_tdx][0].trainIdx;
            // check if they are mutual best matches and the minimum distance
            double dist_nn = lmatches_lr[i][0].distance;
            double dist_12 = lmatches_lr[i][1].distance - lmatches_lr[i][0].distance;
            double length  = lines_r[lr_tdx].lineLength;
            double min_line_length_th = Config::minLineLength() * std::min( cam->getWidth(), cam->getHeight() );

            if( lr_qdx == rl_tdx && dist_nn < nn_dist_th && dist_12 > nn12_dist_th && length > min_line_length_th)
            {
                // check stereo epipolar constraint
                if( fabsf(lines_l[lr_qdx].angle) >= Config::minHorizAngle() && fabsf(lines_r[lr_tdx].angle) >= Config::minHorizAngle() && fabsf(angDiff(lines_l[lr_qdx].angle,lines_r[lr_tdx].angle)) < Config::maxAngleDiff() )
                {
                    // estimate the disparity of the endpoints
                    Vector3d sp_r; sp_r << lines_r[lr_tdx].startPointX, lines_r[lr_tdx].startPointY, 1.0;
                    Vector3d ep_r; ep_r << lines_r[lr_tdx].endPointX,   lines_r[lr_tdx].endPointY,   1.0;
                    Vector3d le_r; le_r << sp_r.cross(ep_r);
                    sp_r << - (le_r(2)+le_r(1)*lines_l[lr_qdx].startPointY )/le_r(0) , lines_l[lr_qdx].startPointY ,  1.0;
                    ep_r << - (le_r(2)+le_r(1)*lines_l[lr_qdx].endPointY   )/le_r(0) , lines_l[lr_qdx].endPointY ,    1.0;
                    double disp_s = lines_l[lr_qdx].startPointX - sp_r(0);
                    double disp_e = lines_l[lr_qdx].endPointX   - ep_r(0);
                    Vector3d sp_l; sp_l << lines_l[lr_qdx].startPointX, lines_l[lr_qdx].startPointY, 1.0;
                    Vector3d ep_l; ep_l << lines_l[lr_qdx].endPointX,   lines_l[lr_qdx].endPointY,   1.0;
                    Vector3d le_l; le_l << sp_l.cross(ep_l); le_l = le_l / sqrt( le_l(0)*le_l(0) + le_l(1)*le_l(1) );
                    // check minimal disparity
                    if( disp_s >= Config::minDisp() && disp_e >= Config::minDisp() && fabsf(le_r(0)) > Config::lineHorizTh() )
                    {
                        ldesc_l_.push_back( ldesc_l.row(lr_qdx) );
                        // ldesc_r_.push_back( ldesc_r.row(jdx) );
                        // cout << endl << le_l.transpose() << "\t\t" << lines_l[lr_qdx].angle * 180.0 / CV_PI << " " << lines_r[lr_tdx].angle * 180.0 / CV_PI ;
                        Vector3d sP_; sP_ = cam->backProjection( sp_l(0), sp_l(1), disp_s);
                        Vector3d eP_; eP_ = cam->backProjection( ep_l(0), ep_l(1), disp_e);
                        stereo_ls.push_back( new LineFeature(Vector2d(sp_l(0),sp_l(1)),disp_s,sP_,Vector2d(ep_l(0),ep_l(1)),disp_e,eP_,le_l) );
                    }
                }
            }
        }
        ldesc_l_.copyTo(ldesc_l);

        s5 += 1000.0 * clk.Tac(); clk.Tic();

    }  

    cout << endl;
    cout << endl << "Detection   (ms): " << s1;
    cout << endl << "Descriptors (ms): " << s2;
    cout << endl << "Thresholds  (ms): " << s3;
    cout << endl << "Re-sorting  (ms): " << s4;
    cout << endl << "Loop        (ms): " << s5;
    cout << endl << "Total       (ms): " << s1+s2+s3+s4+s5;

}

void StereoFrame::detectFeatures(Mat img, vector<KeyPoint> &points, Mat &pdesc, vector<KeyLine> &lines, Mat &ldesc)
{

    // lsd parameters
    LSDDetector::LSDOptions opts;
    opts.refine       = Config::lsdRefine();
    opts.scale        = Config::lsdScale();
    opts.sigma_scale  = Config::lsdSigmaScale();
    opts.quant        = Config::lsdQuant();
    opts.ang_th       = Config::lsdAngTh();
    opts.log_eps      = Config::lsdLogEps();
    opts.density_th   = Config::lsdDensityTh();
    opts.n_bins       = Config::lsdNBins();

    //cout << endl << x << endl;

    // Declare objects
    Ptr<LSDDetector>        lsd = LSDDetector::createLSDDetector();
    Ptr<BinaryDescriptor>   lbd = BinaryDescriptor::createBinaryDescriptor();
    Ptr<ORB>                orb = ORB::create( Config::orbNFeatures(), Config::orbScaleFactor(), Config::orbNLevels() );

    //cout << endl << x << endl;

    // Detect point features
    if( Config::hasPoints() )
        orb->detectAndCompute( img, Mat(), points, pdesc, false);

    //cout << endl << x << endl;

    // Detect line features
    if( Config::hasLines() )
    {
        lsd->detect( img, lines, 1, 1, opts);
        lbd->compute( img, lines, ldesc);
    }

    //cout << endl << x << endl;
}

void StereoFrame::matchPointFeatures(BFMatcher* bfm, Mat pdesc_1, Mat pdesc_2, vector<vector<DMatch>> &pmatches_12  )
{
    bfm->knnMatch( pdesc_1, pdesc_2, pmatches_12, 2);
}

void StereoFrame::matchLineFeatures(Ptr<BinaryDescriptorMatcher> bdm, Mat ldesc_1, Mat ldesc_2, vector<vector<DMatch>> &lmatches_12  )
{
    bdm->knnMatch( ldesc_1, ldesc_2, lmatches_12, 2);
}

void StereoFrame::pointDescriptorMAD( const vector<vector<DMatch>> matches, double &nn_mad, double &nn12_mad )
{

    vector<vector<DMatch>> matches_nn, matches_12;
    matches_nn = matches;
    matches_12 = matches;

    // estimate the NN's distance standard deviation
    double nn_dist_median;
    sort( matches_nn.begin(), matches_nn.end(), compare_descriptor_by_NN_dist() );
    nn_mad = matches_nn[int(matches_nn.size()/2)][0].distance;
    /*for( int j = 0; j < matches_nn.size(); j++)
        matches_nn[j][0].distance = fabsf( matches_nn[j][0].distance - nn_dist_median );
    sort( matches_nn.begin(), matches_nn.end(), compare_descriptor_by_NN_dist() );
    nn_mad = 1.4826 * matches_nn[int(matches_nn.size()/2)][0].distance;*/

    // estimate the NN's 12 distance standard deviation
    double nn12_dist_median;
    sort( matches_12.begin(), matches_12.end(), compare_descriptor_by_NN12_dist() );
    nn12_mad = matches_12[int(matches_12.size()/2)][1].distance - matches_12[int(matches_12.size()/2)][0].distance;
    /*for( int j = 0; j < matches_12.size(); j++)
        matches_12[j][0].distance = fabsf( matches_12[j][1].distance - matches_12[j][0].distance - nn_dist_median );
    sort( matches_12.begin(), matches_12.end(), compare_descriptor_by_NN_dist() );
    nn12_mad = matches_12[int(matches_12.size()/2)][0].distance / 1.4826 ;*/

}

void StereoFrame::lineDescriptorMAD( const vector<vector<DMatch>> matches, double &nn_mad, double &nn12_mad )
{

    vector<vector<DMatch>> matches_nn, matches_12;
    matches_nn = matches;
    matches_12 = matches;

    // estimate the NN's distance standard deviation
    //double nn_dist_median;
    sort( matches_nn.begin(), matches_nn.end(), compare_descriptor_by_NN_dist() );
    nn_mad = matches_nn[int(matches_nn.size()/2)][0].distance;

    /*for( int j = 0; j < matches_nn.size(); j++)
        matches_nn[j][0].distance = fabsf( matches_nn[j][0].distance - nn_dist_median );
    sort( matches_nn.begin(), matches_nn.end(), compare_descriptor_by_NN_dist() );
    nn_mad = 1.4826 * matches_nn[int(matches_nn.size()/2)][0].distance;*/

    // estimate the NN's 12 distance standard deviation
    //double nn12_dist_median;
    sort( matches_12.begin(), matches_12.end(), compare_descriptor_by_NN12_dist() );
    nn12_mad = matches_12[int(matches_12.size()/2)][1].distance - matches_12[int(matches_12.size()/2)][0].distance;
    /*for( int j = 0; j < matches_12.size(); j++)
        matches_12[j][0].distance = fabsf( matches_12[j][1].distance - matches_12[j][0].distance - nn_dist_median );
    sort( matches_12.begin(), matches_12.end(), compare_descriptor_by_NN_dist() );
    nn12_mad = matches_12[int(matches_12.size()/2)][0].distance * 1.4826 ;*/

}

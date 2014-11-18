#include "mono_vo/MonoVO.h"

#include <opencv2/core/eigen.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <ros/ros.h>

#include "cauldron/EigenUtils.h"
#include "fivepoint/fivepoint.hpp"
#include "pose_estimation/P3P.h"

namespace px
{

MonoVO::MonoVO(const CameraSystemConstPtr& cameraSystem,
               int cameraId, bool preUndistort)
 : k_epipolarThresh(0.00005)
 , k_imageMotionThresh(10.0f)
 , k_maxDistanceRatio(0.7f)
 , k_maxStereoRange(100.0)
 , k_nominalFocalLength(300.0)
 , k_preUndistort(preUndistort)
 , k_reprojErrorThresh(2.0)
 , k_sphericalErrorThresh(0.999976)
 , m_cameraSystem(cameraSystem)
 , m_cameraId(cameraId)
 , m_init(false)
 , m_nInlierCorrespondences(0)
 , m_debug(true)
{
    if (k_preUndistort)
    {
        cameraSystem->getCamera(cameraId)->initUndistortMap(m_undistortMapX, m_undistortMapY);

        cameraSystem->getCamera(cameraId)->setZeroDistortion();
    }

    m_lba = boost::make_shared<LocalMonoBA>(cameraSystem, m_cameraId);
}

bool
MonoVO::init(const std::string& detectorType,
             const std::string& descriptorExtractorType,
             const std::string& descriptorMatcherType)
{
    boost::lock_guard<boost::mutex> lock(m_globalMutex);

    m_featureDetector = cv::FeatureDetector::create(detectorType);
    if (!m_featureDetector)
    {
        ROS_ERROR("Failed to create feature detector of type: %s",
                  detectorType.c_str());
        return false;
    }

    m_descriptorExtractor =
        cv::DescriptorExtractor::create(descriptorExtractorType);
    if (!m_descriptorExtractor)
    {
        ROS_ERROR("Failed to create descriptor extractor of type: %s",
                  descriptorExtractorType.c_str());
        return false;
    }

    m_descriptorMatcher = cv::DescriptorMatcher::create(descriptorMatcherType);
    if (!m_descriptorMatcher)
    {
        ROS_ERROR("Failed to create descriptor matcher of type: %s",
                  descriptorMatcherType.c_str());
        return false;
    }

    return true;
}

bool
MonoVO::readFrame(const ros::Time& stamp,
                  const cv::Mat& image)
{
    boost::lock_guard<boost::mutex> lock(m_globalMutex);

    m_imageStamp = stamp;
    image.copyTo(m_image);

    return true;
}

bool
MonoVO::processFrames(FrameSetPtr& frameSet)
{
    boost::lock_guard<boost::mutex> lock(m_globalMutex);

    ros::Time tsStart = ros::Time::now();

    std::vector<cv::KeyPoint> kpts;
    std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > spts;
    cv::Mat dtors;

    ImageMetadata metadata(m_cameraSystem->getCamera(m_cameraId),
                           m_image, m_undistortMapX, m_undistortMapY);

    processFrame(metadata, m_imageProc, kpts, spts, dtors);

    FramePtr frame = boost::make_shared<Frame>();
    frame->cameraId() = m_cameraId;

    for (size_t i = 0; i < kpts.size(); ++i)
    {
        Point2DFeaturePtr feature = boost::make_shared<Point2DFeature>();
        feature->frame() = frame.get();
        feature->keypoint() = kpts.at(i);
        dtors.row(i).copyTo(feature->descriptor());
        feature->ray() = spts.at(i);

        frame->features2D().push_back(feature);
    }

    frameSet = boost::make_shared<FrameSet>();
    frame->frameSet() = frameSet.get();
    frameSet->frames().push_back(frame);

    // Avoid copying image data as image data takes up significant memory.
    if (m_debug)
    {
        m_imageProc.copyTo(frame->image());
    }

    bool replaceCurrentFrameSet = false;
    if (m_frameSetCurr)
    {
        // Current frame set is not keyed. Remove all references to
        // the current frame set.
        for (size_t i = 0; i < m_frameSetPrev->frames().size(); ++i)
        {
            std::vector<Point2DFeaturePtr>& features = m_frameSetPrev->frames().at(i)->features2D();

            for (size_t j = 0; j < features.size(); ++j)
            {
                Point2DFeaturePtr& feature = features.at(j);

                if (!feature->nextMatches().empty())
                {
                    feature->bestNextMatchId() = -1;
                    feature->nextMatches().clear();
                }
            }
        }

        for (size_t i = 0; i < m_frameSetCurr->frames().size(); ++i)
        {
            std::vector<Point2DFeaturePtr>& features = m_frameSetCurr->frames().at(i)->features2D();

            for (size_t j = 0; j < features.size(); ++j)
            {
                Point2DFeature* feature = features.at(j).get();

                if (feature->prevMatches().empty())
                {
                    continue;
                }

                Point3DFeaturePtr& scenePoint = feature->feature3D();

                std::vector<Point2DFeature*>::reverse_iterator it = scenePoint->features2D().rbegin();
                while (it != scenePoint->features2D().rend())
                {
                    if (*it == feature)
                    {
                        scenePoint->features2D().erase(--it.base());
                        break;
                    }

                    ++it;
                }
            }
        }

        replaceCurrentFrameSet = true;
    }

    if (!m_frameSetPrev)
    {
        PosePtr pose = boost::make_shared<Pose>(Eigen::Matrix4d::Identity());
        pose->timeStamp() = m_imageStamp;

        frameSet->systemPose() = pose;

        m_frameSetPrev = frameSet;
    }
    else
    {
        std::vector<cv::DMatch> rawMatches;

        // Find 2D-2D correspondences between previous and current frames.
        match2D2DCorrespondences(m_frameSetPrev, frameSet, rawMatches);

        std::vector<Point2DFeaturePtr>& featuresPrev = m_frameSetPrev->frames().at(0)->features2D();
        std::vector<Point2DFeaturePtr>& featuresCurr = frameSet->frames().at(0)->features2D();

        Eigen::Matrix4d cameraPose;
        std::vector<cv::DMatch> matches;

        if (!m_init)
        {
            Eigen::Matrix4d relativeMotion;
            solve5PointRansac(m_frameSetPrev->frames().at(0), frameSet->frames().at(0),
                              rawMatches, relativeMotion, matches);

            cameraPose = relativeMotion *
                         invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(m_cameraId));

            float dp = 0.0f;
            for (size_t i = 0; i < matches.size(); ++i)
            {
                const cv::DMatch& match = matches.at(i);

                Point2DFeaturePtr& featurePrev = featuresPrev.at(match.queryIdx);
                Point2DFeaturePtr& featureCurr = featuresCurr.at(match.trainIdx);

                dp += cv::norm(featureCurr->keypoint().pt - featurePrev->keypoint().pt);
            }
            dp /= matches.size();

            if (dp > k_imageMotionThresh)
            {
                m_init = true;

                ROS_INFO("Initialized keyframes.");
            }
            else
            {
                return true;
            }

            // For each match, reconstruct the scene point.
            // If the reprojection error of the scene point in either camera exceeds
            // a threshold, remove the corresponding match.
            std::vector<cv::DMatch>::iterator itMatch = matches.begin();
            while (itMatch != matches.end())
            {
                const cv::DMatch& match = *itMatch;

                Point2DFeaturePtr& featurePrev = featuresPrev.at(match.queryIdx);
                Point2DFeaturePtr& featureCurr = featuresCurr.at(match.trainIdx);

                if (reconstructScenePoint(featurePrev, featureCurr, relativeMotion))
                {
//                    {
//                        Eigen::Matrix4d pose = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(m_cameraId));
//
//                        const Point3DFeaturePtr& scenePoint = featurePrev->feature3D();
//
//                        Eigen::Vector3d P = transformPoint(pose, scenePoint->point());
//                        Eigen::Vector2d p;
//                        m_cameraSystem->getCamera(m_cameraId)->spaceToPlane(P, p);
//
//                        double err = (p - Eigen::Vector2d(featurePrev->keypoint().pt.x,
//                                                          featurePrev->keypoint().pt.y)).norm();
//
//                        std::cout << "prev: " << err << std::endl;
//
//                        pose = relativeMotion * invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(m_cameraId));
//
//                        P = transformPoint(pose, scenePoint->point());
//                        m_cameraSystem->getCamera(m_cameraId)->spaceToPlane(P, p);
//
//                        err = (p - Eigen::Vector2d(featureCurr->keypoint().pt.x,
//                                                   featureCurr->keypoint().pt.y)).norm();
//
//                        std::cout << "curr: " << err << std::endl;
//                    }

                    ++itMatch;
                }
                else
                {
                    itMatch = matches.erase(itMatch);
                }
            }
        }
        else
        {
            // Estimate relative pose from P3P RANSAC.
            solveP3PRansac(m_frameSetPrev->frames().at(0), frameSet->frames().at(0),
                           rawMatches, cameraPose, matches);

            for (size_t i = 0; i < matches.size(); ++i)
            {
                const cv::DMatch& match = matches.at(i);

                Point2DFeaturePtr& featurePrev = featuresPrev.at(match.queryIdx);
                Point2DFeaturePtr& featureCurr = featuresCurr.at(match.trainIdx);

                Point3DFeaturePtr& scenePoint = featurePrev->feature3D();

                featureCurr->feature3D() = scenePoint;
                scenePoint->features2D().push_back(featureCurr.get());
            }
        }

        m_nInlierCorrespondences = matches.size();
        if (matches.size() < 10)
        {
            ROS_WARN("# 2D-3D correspondences (%lu) is too low for reliable motion estimation.",
                     matches.size());
        }

        Eigen::Matrix4d systemPose = m_cameraSystem->getGlobalCameraPose(m_cameraId) * cameraPose;

        PosePtr pose = boost::make_shared<Pose>(systemPose);
        pose->timeStamp() = m_imageStamp;

        frameSet->systemPose() = pose;

        for (size_t i = 0; i < matches.size(); ++i)
        {
            const cv::DMatch& match = matches.at(i);

            Point2DFeaturePtr& featurePrev = featuresPrev.at(match.queryIdx);
            Point2DFeaturePtr& featureCurr = featuresCurr.at(match.trainIdx);

            featurePrev->nextMatches().push_back(featureCurr.get());
            featurePrev->bestNextMatchId() = 0;

            featureCurr->prevMatches().push_back(featurePrev.get());
            featureCurr->bestPrevMatchId() = 0;
        }

        // remove singleton feature correspondences
        std::vector<Point2DFeaturePtr>::iterator it = featuresPrev.begin();
        while (it != featuresPrev.end())
        {
            if ((*it)->nextMatches().empty())
            {
                it = featuresPrev.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (m_debug)
        {
            ROS_INFO("Motion estimation took %.3f s.", (ros::Time::now() - tsStart).toSec());

            int nTempCorr = 0;
            const std::vector<Point2DFeaturePtr>& featuresCurr = frameSet->frames().at(0)->features2D();
            for (size_t i = 0; i < featuresCurr.size(); ++i)
            {
                if (!featuresCurr.at(i)->prevMatches().empty())
                {
                    ++nTempCorr;
                }
            }

            ROS_INFO("# temporal correspondences: %d", nTempCorr);

            double avgError, maxError;
            size_t featureCount;
            reprojErrorStats(frameSet, avgError, maxError, featureCount);

            ROS_INFO("Reprojection error before local BA: avg = %.3f | max = %.3f | count = %lu",
                     avgError, maxError, featureCount);
        }

        tsStart = ros::Time::now();

        m_lba->addFrameSet(frameSet, replaceCurrentFrameSet);

        // remove correspondences that have high reprojection errors
        it = featuresCurr.begin();

        Eigen::Matrix4d H_sys_cam = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(frame->cameraId()));

        while (it != featuresCurr.end())
        {
            Point2DFeature* feature = it->get();

            if (feature->prevMatches().empty())
            {
                ++it;
                continue;
            }

            Point3DFeaturePtr& scenePoint = feature->feature3D();
            Eigen::Vector3d P = scenePoint->point();

            Eigen::Matrix4d H = H_sys_cam * frameSet->systemPose()->toMatrix();
            Eigen::Vector3d P_cam = transformPoint(H, P);
            Eigen::Vector3d ray_est = P_cam.normalized();

            bool remove = false;

            double err = fabs(ray_est.dot(feature->ray()));
            if (err < k_sphericalErrorThresh)
            {
                remove = true;
            }

            if (remove)
            {
                Point2DFeature* featurePrev = feature->prevMatch();

                featurePrev->nextMatches().clear();
                featurePrev->bestNextMatchId() = -1;

                bool flag = false;
                std::vector<Point2DFeature*>::reverse_iterator rit = scenePoint->features2D().rbegin();
                while (rit != scenePoint->features2D().rend())
                {
                    if (*rit == feature)
                    {
                        scenePoint->features2D().erase(--rit.base());
                        flag = true;
                    }
                    if (flag)
                    {
                        break;
                    }

                    ++rit;
                }

                it = featuresCurr.erase(it);

                if (featurePrev->prevMatches().empty())
                {
                    std::vector<Point2DFeaturePtr>::iterator itPrev = featuresPrev.begin();
                    while (itPrev != featuresPrev.end())
                    {
                        if (itPrev->get() == featurePrev)
                        {
                            itPrev = featuresPrev.erase(itPrev);
                        }
                        else
                        {
                            ++itPrev;
                        }
                    }
                }
            }
            else
            {
                ++it;
            }
        }

        if (m_debug)
        {
            ROS_INFO("Local BA took %.3f s.", (ros::Time::now() - tsStart).toSec());

            double avgError, maxError;
            size_t featureCount;
            reprojErrorStats(frameSet, avgError, maxError, featureCount);

            ROS_INFO("Reprojection error after local BA: avg = %.3f | max = %.3f | count = %lu",
                     avgError, maxError, featureCount);

            visualizeCorrespondences(m_frameSetPrev, frameSet);
        }

        m_frameSetCurr = frameSet;
    }

    return true;
}

bool
MonoVO::getPreviousPose(Eigen::Matrix4d& pose) const
{
    if (m_frameSetPrev)
    {
        pose = m_frameSetPrev->systemPose()->toMatrix().inverse();

        return true;
    }
    else
    {
        return false;
    }
}

bool
MonoVO::getPreviousPose(geometry_msgs::PoseStampedPtr& pose) const
{
    if (m_frameSetPrev)
    {
        pose = boost::make_shared<geometry_msgs::PoseStamped>();

        pose->header.stamp = m_frameSetPrev->systemPose()->timeStamp();

        Eigen::Matrix4d systemPose_inv = m_frameSetPrev->systemPose()->toMatrix().inverse();

        pose->pose.position.x = systemPose_inv(0,3);
        pose->pose.position.y = systemPose_inv(1,3);
        pose->pose.position.z = systemPose_inv(2,3);

        Eigen::Quaterniond q(systemPose_inv.block<3,3>(0,0));
        pose->pose.orientation.w = q.w();
        pose->pose.orientation.x = q.x();
        pose->pose.orientation.y = q.y();
        pose->pose.orientation.z = q.z();

        return true;
    }
    else
    {
        return false;
    }
}

bool
MonoVO::getCurrentPose(Eigen::Matrix4d& pose) const
{
    if (m_frameSetCurr)
    {
        pose = m_frameSetCurr->systemPose()->toMatrix().inverse();

        return true;
    }
    else
    {
        return false;
    }
}

bool
MonoVO::getCurrentPose(geometry_msgs::PoseStampedPtr& pose) const
{
    if (m_frameSetCurr)
    {
        pose = boost::make_shared<geometry_msgs::PoseStamped>();

        pose->header.stamp = m_frameSetCurr->systemPose()->timeStamp();

        Eigen::Matrix4d systemPose_inv = m_frameSetCurr->systemPose()->toMatrix().inverse();

        pose->pose.position.x = systemPose_inv(0,3);
        pose->pose.position.y = systemPose_inv(1,3);
        pose->pose.position.z = systemPose_inv(2,3);

        Eigen::Quaterniond q(systemPose_inv.block<3,3>(0,0));
        pose->pose.orientation.w = q.w();
        pose->pose.orientation.x = q.x();
        pose->pose.orientation.y = q.y();
        pose->pose.orientation.z = q.z();

        return true;
    }
    else
    {
        return false;
    }
}

size_t
MonoVO::getCurrentInlierCorrespondenceCount(void) const
{
    return m_nInlierCorrespondences;
}

void
MonoVO::keyCurrentFrameSet(void)
{
    if (!m_frameSetCurr)
    {
        return;
    }

    m_frameSetPrev = m_frameSetCurr;
    m_frameSetCurr.reset();

    if (m_debug)
    {
        ROS_INFO("Keyed frameset.");
    }
}

void
MonoVO::getDescriptorMat(const FrameConstPtr& frame, cv::Mat& dmat) const
{
    const std::vector<Point2DFeaturePtr>& features2D = frame->features2D();

    if (features2D.empty())
    {
        dmat = cv::Mat();

        return;
    }

    dmat = cv::Mat(features2D.size(), features2D.front()->descriptor().cols,
                   features2D.front()->descriptor().type());

    for (size_t i = 0; i < features2D.size(); ++i)
    {
        const Point2DFeatureConstPtr& feature2D = features2D.at(i);

        feature2D->descriptor().copyTo(dmat.row(i));
    }
}

void
MonoVO::getDescriptorMatVec(const FrameSetConstPtr& frameSet,
                              std::vector<cv::Mat>& dmatVec) const
{
    for (size_t i = 0; i < frameSet->frames().size(); ++i)
    {
        cv::Mat dmat;
        getDescriptorMat(frameSet->frames().at(i), dmat);

        dmatVec.push_back(dmat);
    }
}

void
MonoVO::matchDescriptors(const cv::Mat& queryDescriptors,
                           const cv::Mat& trainDescriptors,
                           std::vector<cv::DMatch>& matches,
                           const cv::Mat& mask,
                           DescriptorMatchMethod matchMethod,
                           float matchParam) const
{
    matches.clear();

    switch (matchMethod)
    {
    case RATIO_MATCH:
    {
        std::vector<std::vector<cv::DMatch> > rawMatches;
        m_descriptorMatcher->knnMatch(queryDescriptors, trainDescriptors,
                                      rawMatches, 2, mask, true);

        matches.reserve(rawMatches.size());
        for (size_t i = 0; i < rawMatches.size(); ++i)
        {
            const std::vector<cv::DMatch>& rawMatch = rawMatches.at(i);

            if (rawMatch.size() < 2)
            {
                continue;
            }

            float distanceRatio = rawMatch.at(0).distance / rawMatch.at(1).distance;

            if (distanceRatio > matchParam)
            {
                continue;
            }

            matches.push_back(rawMatch.at(0));
        }
        break;
    }
    case BEST_MATCH:
    default:
    {
        m_descriptorMatcher->match(queryDescriptors, trainDescriptors,
                                   matches, mask);
    }
    }
}

void
MonoVO::match2D2DCorrespondences(const FrameSetConstPtr& frameSet1,
                                 const FrameSetConstPtr& frameSet2,
                                 std::vector<cv::DMatch>& matches) const
{
    matches.clear();

    std::vector<cv::Mat> dtors1;
    getDescriptorMatVec(frameSet1, dtors1);

    std::vector<cv::Mat> dtors2;
    getDescriptorMatVec(frameSet2, dtors2);

    // match between image in frame set 1 and image in frame set 2
    matchDescriptors(dtors1.at(0), dtors2.at(0), matches,
                     cv::Mat(), RATIO_MATCH, k_maxDistanceRatio);
}

void
MonoVO::processFrame(const ImageMetadata& metadata,
                     cv::Mat& imageProc,
                     std::vector<cv::KeyPoint>& kpts,
                     std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> >& spts,
                     cv::Mat& dtors) const
{
    if (k_preUndistort)
    {
        // Undistort images so that we avoid the computationally expensive step of
        // applying distortion and undistortion in projection and backprojection
        // respectively.
        cv::remap(metadata.image, imageProc,
                  metadata.undistortMapX, metadata.undistortMapY,
                  cv::INTER_LINEAR);
    }
    else
    {
        metadata.image.copyTo(imageProc);
    }

    // Detect features.
    m_featureDetector->detect(imageProc, kpts, cv::Mat());

    // Backproject feature coordinates to rays with spherical coordinates.
    spts.resize(kpts.size());
    for (size_t i = 0; i < kpts.size(); ++i)
    {
        const cv::KeyPoint& kpt = kpts.at(i);

        metadata.cam->liftSphere(Eigen::Vector2d(kpt.pt.x, kpt.pt.y), spts.at(i));
    }

    m_descriptorExtractor->compute(imageProc, kpts, dtors);
}

bool
MonoVO::reconstructScenePoint(Point2DFeaturePtr& f1,
                              Point2DFeaturePtr& f2,
                              const Eigen::Matrix4d& H) const
{
    Frame* frame1 = f1->frame();

    Eigen::MatrixXd A(3,2);
    A.col(0) = H.block<3,3>(0,0) * f1->ray();
    A.col(1) = - f2->ray();

    Eigen::Vector3d b = - H.block<3,1>(0,3);

    Eigen::Vector2d gamma = A.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b);

    // check if scene point is behind camera
    if (gamma(0) < 0.0 || gamma(1) < 0.0)
    {
        return false;
    }

    // check if scene point is outside allowable stereo range
    if (gamma(0) > k_maxStereoRange)
    {
        return false;
    }

    Eigen::Vector3d P1 = gamma(0) * f1->ray();
    Eigen::Vector3d P2 = H.block<3,3>(0,0) * P1 + H.block<3,1>(0,3);

    if (P2(2) < 0.0)
    {
        return false;
    }

    Eigen::Vector3d ray2_est = P2.normalized();

    double err = fabs(ray2_est.dot(f2->ray()));
    if (err < k_sphericalErrorThresh)
    {
        return false;
    }

    Point3DFeaturePtr p3D = boost::make_shared<Point3DFeature>();
    p3D->point() = transformPoint(m_cameraSystem->getGlobalCameraPose(frame1->cameraId()), P1);
    p3D->features2D().push_back(f1.get());
    p3D->features2D().push_back(f2.get());

    f1->feature3D() = p3D;
    f2->feature3D() = p3D;

    return true;
}

void
MonoVO::reprojErrorStats(const FrameSetConstPtr& frameSet,
                         double& avgError, double& maxError,
                         size_t& featureCount) const
{
    const FrameConstPtr& frame = frameSet->frames().at(0);

    const CameraConstPtr& cam = m_cameraSystem->getCamera(m_cameraId);

    Eigen::Matrix4d pose = invertHomogeneousTransform(m_cameraSystem->getGlobalCameraPose(m_cameraId)) *
                           frameSet->systemPose()->toMatrix();

    size_t count = 0;
    double sumError = 0.0;
    maxError = 0.0;
    for (size_t i = 0; i < frame->features2D().size(); ++i)
    {
        const Point2DFeaturePtr& feature = frame->features2D().at(i);

        if (feature->prevMatches().empty())
        {
            continue;
        }

        const Point3DFeaturePtr& scenePoint = feature->feature3D();

        Eigen::Vector3d P = transformPoint(pose, scenePoint->point());
        Eigen::Vector2d p;
        cam->spaceToPlane(P, p);

        double err = (p - Eigen::Vector2d(feature->keypoint().pt.x,
                                          feature->keypoint().pt.y)).norm();

        ++count;
        sumError += err;

        if (maxError < err)
        {
            maxError = err;
        }
    }

    if (count == 0)
    {
        avgError = 0.0;
        featureCount = 0;
    }
    else
    {
        avgError = sumError / static_cast<double>(count);
        featureCount = count;
    }
}

void
MonoVO::solve5PointRansac(const FrameConstPtr& frame1,
                          const FrameConstPtr& frame2,
                          const std::vector<cv::DMatch>& matches,
                          Eigen::Matrix4d& relativeMotion,
                          std::vector<cv::DMatch>& inliers) const
{
    inliers.clear();

    std::vector<cv::Point2f> imagePoints[2];
    for (size_t i = 0; i < matches.size(); ++i)
    {
        const cv::DMatch& match = matches.at(i);

        Eigen::Vector3d ray;

        ray = frame1->features2D().at(match.queryIdx)->ray();
        ray /= ray(2);

        imagePoints[0].push_back(cv::Point2f(ray(0), ray(1)));

        ray = frame2->features2D().at(match.trainIdx)->ray();
        ray /= ray(2);

        imagePoints[1].push_back(cv::Point2f(ray(0), ray(1)));
    }

    cv::Mat inlierMat;
    cv::Mat E = findEssentialMat(imagePoints[0], imagePoints[1], 1.0,
                                 cv::Point2d(0.0, 0.0), CV_FM_RANSAC, 0.99,
                                 k_reprojErrorThresh / k_nominalFocalLength,
                                 100, inlierMat);
    cv::Mat R_cv, t_cv;
    recoverPose(E, imagePoints[0], imagePoints[1],
                R_cv, t_cv, 1.0, cv::Point2d(0.0, 0.0), inlierMat);

    Eigen::Matrix3d R;
    cv::cv2eigen(R_cv, R);

    Eigen::Vector3d t;
    cv::cv2eigen(t_cv, t);

    relativeMotion.setIdentity();
    relativeMotion.block<3,3>(0,0) = R;
    relativeMotion.block<3,1>(0,3) = t;

    for (int i = 0; i < inlierMat.cols; ++i)
    {
        if (!inlierMat.at<unsigned char>(0,i))
        {
            continue;
        }

        inliers.push_back(matches.at(i));
    }
}

void
MonoVO::solveP3PRansac(const FrameConstPtr& frame1,
                       const FrameConstPtr& frame2,
                       const std::vector<cv::DMatch>& matches,
                       Eigen::Matrix4d& H,
                       std::vector<cv::DMatch>& inliers) const
{
    inliers.clear();

    double p = 0.99; // probability that at least one set of random samples does not contain an outlier
    double v = 0.6; // probability of observing an outlier

    double u = 1.0 - v;
    int N = static_cast<int>(log(1.0 - p) / log(1.0 - u * u * u) + 0.5);

    std::vector<size_t> indices;
    for (size_t i = 0; i < matches.size(); ++i)
    {
        indices.push_back(i);
    }

    const std::vector<Point2DFeaturePtr>& features1 = frame1->features2D();
    const std::vector<Point2DFeaturePtr>& features2 = frame2->features2D();

    // run RANSAC to find best H
    Eigen::Matrix4d H_best;
    std::vector<size_t> inlierIds_best;
    for (int i = 0; i < N; ++i)
    {
        std::random_shuffle(indices.begin(), indices.end());

        std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > rays(3);
        std::vector<Eigen::Vector3d, Eigen::aligned_allocator<Eigen::Vector3d> > worldPoints(3);
        for (int j = 0; j < 3; ++j)
        {
            const cv::DMatch& match = matches.at(indices.at(j));

            worldPoints.at(j) = features1.at(match.queryIdx)->feature3D()->point();
            rays.at(j) = features2.at(match.trainIdx)->ray();
        }

        std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d> > H;
        if (!solveP3P(rays, worldPoints, H))
        {
            continue;
        }

        for (size_t j = 0; j < H.size(); ++j)
        {
            Eigen::Matrix4d H_inv = invertHomogeneousTransform(H.at(j));

            std::vector<size_t> inliersIds;
            for (size_t k = 0; k < matches.size(); ++k)
            {
                const cv::DMatch& match = matches.at(k);

                Eigen::Vector3d P_1 = features1.at(match.queryIdx)->feature3D()->point();

                Eigen::Vector3d P_2_pred = transformPoint(H_inv, P_1);

                const Point2DFeatureConstPtr& f2 = features2.at(match.trainIdx);

                double err = fabs(P_2_pred.normalized().dot(f2->ray()));
                if (err < k_sphericalErrorThresh)
                {
                    continue;
                }

                inliersIds.push_back(k);
            }

            if (inliersIds.size() > inlierIds_best.size())
            {
                H_best = H_inv;
                inlierIds_best = inliersIds;
            }
        }
    }

    for (size_t i = 0; i < inlierIds_best.size(); ++i)
    {
        inliers.push_back(matches.at(inlierIds_best.at(i)));
    }

    H = H_best;
}

void
MonoVO::visualizeCorrespondences(const FrameSetConstPtr& frameSetPrev,
                                 const FrameSetConstPtr& frameSetCurr) const
{
    cv::Mat sketch(m_imageProc.rows, m_imageProc.cols, CV_8UC3);

    cv::Mat image(sketch, cv::Rect(0, 0, m_imageProc.cols, m_imageProc.rows));
    cv::cvtColor(m_imageProc, image, CV_GRAY2BGR);

    const std::vector<Point2DFeaturePtr>& featuresCurr = frameSetCurr->frames().at(0)->features2D();

    const int draw_shift_bits = 4;
    const int draw_multiplier = 1 << draw_shift_bits;
    const int radius = 3 * draw_multiplier;

    Eigen::Matrix4d systemPose = frameSetCurr->systemPose()->toMatrix();

    for (size_t i = 0; i < featuresCurr.size(); ++i)
    {
        Point2DFeature* feature = featuresCurr.at(i).get();

        if (!feature->feature3D())
        {
            continue;
        }

        Eigen::Vector3d P = transformPoint(systemPose, feature->feature3D()->point());
        double range = P.norm();
        float r, g, b;
        colormap("jet", fmin(range, 10.0) / 10.0 * 128.0, r, g, b);

        cv::Scalar color(r * 256.0f, g * 256.0f, b * 256.0f);

        std::vector<cv::Point2f> pts;
        while (feature)
        {
            pts.push_back(feature->keypoint().pt);

            if (feature->prevMatches().empty())
            {
                feature = 0;
            }
            else
            {
                feature = feature->prevMatch();
            }
        }

        if (pts.size() > 1)
        {
            cv::Point p(cvRound(pts.at(0).x * draw_multiplier),
                        cvRound(pts.at(0).y * draw_multiplier));
            cv::circle(image, p, radius, color, 1, CV_AA, draw_shift_bits);

            for (size_t j = 0; j < pts.size() - 1; ++j)
            {
                cv::Point p1(cvRound(pts.at(j).x * draw_multiplier),
                             cvRound(pts.at(j).y * draw_multiplier));

                cv::Point p2(cvRound(pts.at(j + 1).x * draw_multiplier),
                             cvRound(pts.at(j + 1).y * draw_multiplier));

                cv::line(image, p1, p2, color, 1, CV_AA, draw_shift_bits);
            }
        }
    }

    cv::imshow("Correspondences", sketch);
    cv::waitKey(2);
}

}

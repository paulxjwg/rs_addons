#include <uima/api.hpp>

#include <pcl/point_types.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/passthrough.h>
#include <pcl/features/shot_omp.h>
#include <pcl/registration/ia_ransac.h>
#include <pcl/features/fpfh.h>

#include <flann/flann.h>
#include <flann/io/hdf5.h>

//Caffe
#include <caffe/caffe.hpp>

//RS
#include <rs/types/all_types.h>
#include <rs/scene_cas.h>
#include <rs/utils/time.h>
#include <rs/DrawingAnnotator.h>

#include <ros/package.h>

#include <rs_addons/CaffeProxy.h>

#define CAFFE_DIR "/home/balintbe/local/src/caffe"
#define CAFFE_MODEL_FILE CAFFE_DIR "/models/bvlc_reference_caffenet/deploy.prototxt"
#define CAFFE_TRAINED_FILE CAFFE_DIR "/models/bvlc_reference_caffenet/bvlc_reference_caffenet.caffemodel"
#define CAFFE_MEAN_FILE CAFFE_DIR "/data/ilsvrc12/imagenet_mean.binaryproto"
#define CAFFE_LABLE_FILE CAFFE_DIR "/data/ilsvrc12/synset_words.txt"

using namespace uima;

class DeCafClassifier : public DrawingAnnotator
{
private:
  typedef std::pair<std::string, std::vector<float> > Model;
  typedef flann::Index<flann::ChiSquareDistance<float> > Index;

  std::string packagePath, h5_file, list_file, kdtree_file;
  std::vector<Model> models;
  cv::Mat data;
  CaffeProxy caffeProxyObj;
  cv::flann::Index index;

  int k;

  cv::Mat color;
  pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud;
public:

  DeCafClassifier(): DrawingAnnotator(__func__), caffeProxyObj(CAFFE_MODEL_FILE, CAFFE_TRAINED_FILE, CAFFE_MEAN_FILE, CAFFE_LABLE_FILE)
  {
    cloud = pcl::PointCloud<pcl::PointXYZRGBA>::Ptr(new pcl::PointCloud<pcl::PointXYZRGBA>);
  }

  TyErrorId initialize(AnnotatorContext &ctx)
  {
    outInfo("initialize");
    packagePath = ros::package::getPath("rs_addons") + '/';
    ctx.extractValue("DeCafH5File", h5_file);
    ctx.extractValue("DeCafListFile", list_file);
    ctx.extractValue("DeCafKDTreeIndices", kdtree_file);
    ctx.extractValue("DeCafKNeighbors", k);

    outInfo(h5_file);
    outInfo(list_file);
    outInfo(kdtree_file);

    // Check if the data has already been saved to disk
    if(!boost::filesystem::exists(packagePath + h5_file) || !boost::filesystem::exists(packagePath + list_file) || !boost::filesystem::exists(packagePath + kdtree_file))
    {
      outError("files not found!");
      return UIMA_ERR_USER_ANNOTATOR_COULD_NOT_INIT;
    }

    flann::Matrix<float> data;
    loadFileList(models, packagePath + list_file);
    flann::load_from_file(data, packagePath + h5_file, "training_data");
    outInfo("Training data found. Loaded " << data.rows << " models from " << h5_file << "/" << list_file);

    this->data = cv::Mat(data.rows, data.cols, CV_32F, data.ptr()).clone();
    index.build(this->data, cv::flann::KDTreeIndexParams());

    return UIMA_ERR_NONE;
  }

  TyErrorId destroy()
  {
    outInfo("destroy");
    return UIMA_ERR_NONE;
  }

  TyErrorId processWithLock(CAS &tcas, ResultSpecification const &res_spec)
  {
    MEASURE_TIME;
    outInfo("process start");
    rs::SceneCas cas(tcas);

    cas.get(VIEW_CLOUD, *cloud);
    cas.get(VIEW_COLOR_IMAGE, color);

    rs::Scene scene = cas.getScene();

    std::vector<rs::Cluster> clusters;
    scene.identifiables.filter(clusters);
    for(int i = 0; i < clusters.size(); ++i)
    {
      rs::Cluster &cluster = clusters[i];
      if(!cluster.points.has())
      {
        continue;
      }
      const std::string &name = "cluster" + std::to_string(i);
      cv::Rect roi;
      rs::conversion::from(cluster.rois().roi(), roi);

      const cv::Mat &clusterImg = color(roi);

      Model feature(name, caffeProxyObj.extractFeature(clusterImg));

      std::vector<int> k_indices;
      std::vector<float> k_distances;

      nearestKSearch(index, feature, k, k_indices, k_distances);

      outInfo("The closest " << k << " neighbors for cluser " << i << " are:");
      for(int j = 0; j < k; ++j)
      {
        outInfo("    " << j << " - " << models[k_indices[j]].first << " (" << k_indices[j] << ") with a distance of: " << k_distances[j]);
      }

      rs::Detection detection = rs::create<rs::Detection>(tcas);
      detection.name.set( models[k_indices[0]].first);
      detection.source.set("DeCafClassifier");
      detection.confidence.set(k_distances[0]);
      cluster.annotations.append(detection);

      rs::ImageROI image_roi = cluster.rois();
      cv::Rect rect;
      rs::conversion::from(image_roi.roi(), rect);
      drawCluster(rect, models[k_indices[0]].first);
    }

    return UIMA_ERR_NONE;
  }
  void drawCluster(cv::Rect roi, const std::string &label)
  {
    cv::rectangle(color, roi, CV_RGB(255, 0, 0));
    int offset = 7;
      int baseLine;
      cv::Size textSize = cv::getTextSize(label, cv::FONT_HERSHEY_PLAIN, 0.8, 1, &baseLine);
      cv::putText(color, label, cv::Point(roi.x + (roi.width - textSize.width) / 2, roi.y - offset - textSize.height), cv::FONT_HERSHEY_PLAIN, 0.8, CV_RGB(255, 255, 200), 1.0);
  }

  void drawImageWithLock(cv::Mat &disp)
  {
    disp = color.clone();
  }

  void fillVisualizerWithLock(pcl::visualization::PCLVisualizer &visualizer, bool firstRun)
  {
    if(firstRun)
    {
      visualizer.addPointCloud(cloud, "cloud");
      visualizer.setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1.0, "cloud");
    }
    else
    {
      visualizer.updatePointCloud(cloud, "cloud");
      visualizer.setPointCloudRenderingProperties(pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 1.0, "cloud");
    }
  }

  inline void nearestKSearch(cv::flann::Index &index, const Model &model, int k, std::vector<int> &indices, std::vector<float> &distances)
  {
    indices.resize(k);
    distances.resize(k);
    index.knnSearch(model.second, indices, distances, k, cv::flann::SearchParams(512));
  }

  bool loadFileList(std::vector<Model> &models, const std::string &filename)
  {
    ifstream fs;
    fs.open(filename.c_str());
    if(!fs.is_open() || fs.fail())
    {
      return (false);
    }

    std::string line;
    while(!fs.eof())
    {
      getline(fs, line);
      if(line.empty())
      {
        continue;
      }
      Model m;
      m.first = line;
      models.push_back(m);
    }
    fs.close();
    return (true);
  }
};

MAKE_AE(DeCafClassifier)

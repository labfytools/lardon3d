/* WHY: this external-only tool freezes calibration observations and reports
 * evidence before Lardon3D's bounded importer is invoked. CONTRACT: it never
 * links runtime sources, writes Project DB, creates L3DCALB1, or starts SfM.
 * INVARIANT: CPU1, canonical SHA ordering and hexfloat reports make equivalent
 * inputs byte-stable; any scientific failure exits without a report. */
#include <opencv2/calib.hpp>
#include <opencv2/core.hpp>
#include <opencv2/geometry/3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/aruco_board.hpp>
#include <opencv2/objdetect/aruco_detector.hpp>
#include <opencv2/objdetect/charuco_detector.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <sys/utsname.h>
#include <vector>

namespace {
constexpr size_t kMaxViews = 4096;
constexpr size_t kMaxCornersPerView = 48;
constexpr size_t kMaxObservations = kMaxViews * kMaxCornersPerView;
constexpr uintmax_t kMaxSourceBytes = 128U * 1024U * 1024U;
constexpr int kMaxDimension = 8192;
constexpr int kSeed = 0x4c334456;
constexpr double kSquare = .030;
constexpr double kMarker = .021;
constexpr int kSquaresX = 9, kSquaresY = 7;

/* WHY: Science v1 requires physical evidence before a pixel observation may
 * reach the solver.  CONTRACT: these fields are a bounded external manifest,
 * not an L3DCALB1 format and never a Project DB representation. */
struct SessionEvidence {
  std::string target_id, instrument, decoder, decoder_version;
  std::array<unsigned char, 32> generator_hash{}, planarity_hash{};
  double resolution_mm = -1, planarity_pass = 0;
  std::array<double, 10> measurements{};
  bool target = false, measurement = false, planarity = false;
  bool decoder_set = false, optical_state = false;
  std::string optical_state_fields;
};

struct View {
  std::string path;
  std::array<unsigned char, 32> hash{};
  int orientation = 0;
  bool accepted = false;
  std::string reject;
  int quadrant = 4, distance_band = 0, angle_class = -1;
  bool holdout = false;
  int width = 0, height = 0;
  std::vector<int> ids;
  /* CONTRACT: Point2f/Point3f are the sole qualified OpenCV API transport.
   * INVARIANT: the paired doubles are an exact promotion, used only for the
   * authoritative independent binary64 residual path. */
  std::vector<cv::Point2f> api_image;
  std::vector<cv::Point3f> api_object;
  std::vector<cv::Point2d> image;
  std::vector<cv::Point3d> object;
  std::vector<cv::Point2d> residuals;
  double occupancy = 0, angle = 0, distance = 0, rmse = 0, max_residual = 0;
  double image_conversion_max = 0, object_conversion_max = 0;
  double pre_solve_corner_rms = -1, clipping_fraction = -1;
  int coordinate_width = 0, coordinate_height = 0;
  size_t coordinate_points = 0, coordinate_comparison_count = 0;
  bool coordinate_declared = false, coordinate_ok = false;
  double coordinate_dx_max = 0, coordinate_dy_max = 0;
  double measured_distance = -1;
  int declared_distance_band = -1;
  unsigned coordinate_coverage = 0;
};
struct Params { double fx=0, fy=0, cx=0, cy=0, k1=0, k2=0, p1=0, p2=0; };
struct Solve { Params p; std::vector<cv::Vec3d> rvec, tvec; double rms=0, maximum=0; size_t high=0, count=0; };

bool hex(const std::string& text, std::array<unsigned char,32>* out) {
  if (text.size()!=64) return false;
  auto value=[](char c)->int { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
  for(size_t i=0;i<32;++i) { int a=value(text[2*i]), b=value(text[2*i+1]); if(a<0||b<0)return false; (*out)[i]=static_cast<unsigned char>((a<<4)|b); }
  return true;
}
std::string hex(const std::array<unsigned char,32>& in) {
  std::ostringstream o; o.imbue(std::locale::classic()); o<<std::hex<<std::setfill('0'); for(auto b:in)o<<std::setw(2)<<static_cast<unsigned>(b); return o.str();
}
bool file_hash(const std::string& path, std::array<unsigned char,32>* out) {
  std::ifstream f(path, std::ios::binary); if(!f) return false; EVP_MD_CTX* c=EVP_MD_CTX_new(); if(!c)return false;
  bool ok=EVP_DigestInit_ex(c,EVP_sha256(),nullptr)==1; std::array<char,65536> b{};
  while(ok&&f.good()){f.read(b.data(),static_cast<std::streamsize>(b.size()));std::streamsize n=f.gcount();if(n>0)ok=EVP_DigestUpdate(c,b.data(),static_cast<size_t>(n))==1;}
  unsigned n=0; ok=ok&&EVP_DigestFinal_ex(c,out->data(),&n)==1&&n==32; EVP_MD_CTX_free(c); return ok;
}
std::array<unsigned char,32> text_hash(const std::string& text) {
  std::array<unsigned char,32> out{}; unsigned n=0;
  if(EVP_Digest(text.data(),text.size(),out.data(),&n,EVP_sha256(),nullptr)!=1||n!=out.size()) std::abort();
  return out;
}
bool finite(double x) { return std::isfinite(x); }
bool json_token(const std::string& x) {
  if (x.empty()) return false;
  for (unsigned char c : x) if (!(std::isalnum(c) || c=='_' || c=='-' || c=='.' || c==':')) return false;
  return true;
}
cv::Mat orient(cv::Mat in,int o) {
  cv::Mat out; if(o==0)return in; if(o==90)cv::rotate(in,out,cv::ROTATE_90_CLOCKWISE); else if(o==180)cv::rotate(in,out,cv::ROTATE_180); else cv::rotate(in,out,cv::ROTATE_90_COUNTERCLOCKWISE); return out;
}
cv::Point3d object_point(int id) {
  if(id<0||id>=48) return {NAN,NAN,NAN};
  return {static_cast<double>(id%(kSquaresX-1)+1)*kSquare, static_cast<double>(id/(kSquaresX-1)+1)*kSquare, 0.0};
}
View* find_view(std::vector<View>* views, const std::array<unsigned char, 32>& hash) {
  for (View& v : *views) if (v.hash == hash) return &v;
  return nullptr;
}
bool read_session(const std::string& path,std::vector<View>* views,std::array<unsigned char,32>* optical,
                  SessionEvidence* evidence,std::string* why) {
  std::ifstream f(path); std::string line, tag; if(!std::getline(f,line)||line!="L3DCAL_SESSION_V1"){*why="bad session magic";return false;} bool have=false;
  while(std::getline(f,line)){if(line.empty())continue;std::istringstream s(line);s.imbue(std::locale::classic());s>>tag;
    if(tag=="optical_sha256"){std::string h;if(have||!(s>>h)||!hex(h,optical)){*why="bad optical hash";return false;}have=true;}
    else if(tag=="image"){View v;std::string h;if(!(s>>v.path>>h>>v.orientation)||!hex(h,&v.hash)||(v.orientation!=0&&v.orientation!=90&&v.orientation!=180&&v.orientation!=270)||v.path.size()>4096){*why="bad image record";return false;}views->push_back(v);}
    else if(tag=="target") { std::string h, dict; int x=0,y=0; double sq=0, mark=0;
      if(evidence->target || !(s>>evidence->target_id>>h>>dict>>x>>y>>sq>>mark) || !hex(h,&evidence->generator_hash) ||
         evidence->target_id.size()>128 || !json_token(evidence->target_id) || dict!="DICT_5X5_100" || x!=9 || y!=7 || sq!=30.0 || mark!=21.0) {*why="target evidence";return false;} evidence->target=true;
    } else if(tag=="measurement") { if(evidence->measurement || !(s>>evidence->instrument>>evidence->resolution_mm) || evidence->instrument.size()>128 || !json_token(evidence->instrument) || !finite(evidence->resolution_mm) || evidence->resolution_mm<=0 || evidence->resolution_mm>.1) {*why="measurement instrument/resolution";return false;}
      for(double& m:evidence->measurements) {
        if(!(s>>m)||!finite(m)){*why="measurement values";return false;}
      }
      evidence->measurement=true;
    } else if(tag=="planarity") { std::string result,h; if(evidence->planarity || !(s>>result>>h) || !hex(h,&evidence->planarity_hash) || (result!="PASS"&&result!="FAIL")) {*why="planarity evidence";return false;} evidence->planarity=true; evidence->planarity_pass=result=="PASS";
    } else if(tag=="decoder") { if(evidence->decoder_set || !(s>>evidence->decoder>>evidence->decoder_version) || evidence->decoder.size()>128 || evidence->decoder_version.size()>128 || !json_token(evidence->decoder) || !json_token(evidence->decoder_version)) {*why="decoder identity/version";return false;} evidence->decoder_set=true;
    } else if(tag=="optical_state") { std::string h; if(evidence->optical_state || !(s>>h>>evidence->optical_state_fields) || !hex(h,optical) || evidence->optical_state_fields.size()>1024 || !json_token(evidence->optical_state_fields) || evidence->optical_state_fields=="UNKNOWN") {*why="optical-state evidence";return false;} evidence->optical_state=true; have=true;
    } else if(tag=="pre_solve") { std::string h; std::array<unsigned char,32> a{}; double rms=0; if(!(s>>h>>rms)||!hex(h,&a)||!finite(rms)||rms<0){*why="pre-solve evidence";return false;} View* v=find_view(views,a);if(!v||v->pre_solve_corner_rms>=0){*why="pre-solve source";return false;}v->pre_solve_corner_rms=rms;
    } else if(tag=="clipping") { std::string h; std::array<unsigned char,32> a{}; double fraction=0; if(!(s>>h>>fraction)||!hex(h,&a)||!finite(fraction)||fraction<0||fraction>1){*why="clipping evidence";return false;} View* v=find_view(views,a);if(!v||v->clipping_fraction>=0){*why="clipping source";return false;}v->clipping_fraction=fraction;
    } else if(tag=="coordinate") { std::string h, decoder, version; std::array<unsigned char,32> a{}; int orient=0,w=0,hh=0; size_t n=0; double dx=0,dy=0; if(!(s>>h>>decoder>>version>>orient>>w>>hh>>n>>dx>>dy)||!hex(h,&a)||decoder!=evidence->decoder||version!=evidence->decoder_version||!(orient==0||orient==90||orient==180||orient==270)||w<=0||hh<=0||n<20||n>kMaxCornersPerView||!finite(dx)||!finite(dy)||dx<0||dy<0){*why="coordinate evidence";return false;} View*v=find_view(views,a);if(!v||v->coordinate_declared||orient!=v->orientation){*why="coordinate source/orientation";return false;}v->coordinate_declared=true;v->coordinate_width=w;v->coordinate_height=hh;v->coordinate_points=n;v->coordinate_comparison_count=n;v->coordinate_dx_max=dx;v->coordinate_dy_max=dy;
    } else if(tag=="coordinate_point") { std::string h,coverage;std::array<unsigned char,32>a{};double sx=0,sy=0,fx=0,fy=0;if(!(s>>h>>coverage>>sx>>sy>>fx>>fy)||!hex(h,&a)||!finite(sx)||!finite(sy)||!finite(fx)||!finite(fy)){*why="coordinate point";return false;}View*v=find_view(views,a);if(!v||!v->coordinate_declared||v->coordinate_points==0){*why="coordinate point source";return false;}const std::array<std::string,9> labels{{"center","top","right","bottom","left","top_left","top_right","bottom_left","bottom_right"}};auto it=std::find(labels.begin(),labels.end(),coverage);if(it==labels.end()){*why="coordinate coverage";return false;}v->coordinate_coverage|=1U<<static_cast<unsigned>(it-labels.begin());const double dx=std::abs(sx-fx),dy=std::abs(sy-fy);v->coordinate_dx_max=std::max(v->coordinate_dx_max,dx);v->coordinate_dy_max=std::max(v->coordinate_dy_max,dy);--v->coordinate_points; }
    else if(tag=="distance") { std::string h;std::array<unsigned char,32>a{};double meters=0;int band=-1;if(!(s>>h>>meters>>band)||!hex(h,&a)||!finite(meters)||meters<=0||band<0||band>2){*why="distance evidence";return false;}View*v=find_view(views,a);if(!v||v->measured_distance>0){*why="distance source";return false;}v->measured_distance=meters;v->declared_distance_band=band; }
    else {*why="unknown session record";return false;}
  }
  if(!have||!evidence->target||!evidence->measurement||!evidence->planarity||!evidence->planarity_pass||!evidence->decoder_set||!evidence->optical_state||views->empty()||views->size()>kMaxViews){*why="required session evidence";return false;}
  double lo=std::numeric_limits<double>::infinity(),hi=-lo;for(double m:evidence->measurements){lo=std::min(lo,m);hi=std::max(hi,m);if(std::abs(m-30.0)>.30){*why="target measurement tolerance";return false;}}if(hi-lo>.20){*why="target measurement range";return false;}
  std::sort(views->begin(),views->end(),[](const View&a,const View&b){return a.hash<b.hash;});
  for(size_t i=1;i<views->size();++i) {
    if((*views)[i-1].hash==(*views)[i].hash){*why="duplicate source hash";return false;}
  }
  for(const View& v:*views) if(v.pre_solve_corner_rms<0||v.clipping_fraction<0||!v.coordinate_declared||v.coordinate_points!=0||v.measured_distance<=0){*why="missing per-view evidence";return false;}
  return true;
}
bool detect(std::vector<View>* views,std::string* why) {
  auto dict=cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_100);
  cv::aruco::CharucoBoard board(cv::Size(kSquaresX,kSquaresY),static_cast<float>(kSquare),static_cast<float>(kMarker),dict); board.setLegacyPattern(false);
  cv::aruco::CharucoParameters cp; cp.minMarkers=2; cp.tryRefineMarkers=false; cp.checkMarkers=true;
  cv::aruco::DetectorParameters dp; cv::aruco::RefineParameters rp; cv::aruco::CharucoDetector detector(board,cp,dp,rp);
  for(View&v:*views){std::error_code ec;const auto bytes=std::filesystem::file_size(v.path,ec);if(ec||bytes>kMaxSourceBytes){v.reject="source_size";continue;}std::array<unsigned char,32> actual{};if(!file_hash(v.path,&actual)||actual!=v.hash){*why="source hash mismatch";return false;}
    cv::Mat raw=cv::imread(v.path,cv::IMREAD_GRAYSCALE|cv::IMREAD_IGNORE_ORIENTATION);if(raw.empty()){v.reject="decode";continue;}cv::Mat image=orient(raw,v.orientation);
    std::vector<cv::Point2f> corners;std::vector<int> ids;std::vector<std::vector<cv::Point2f>> markers;std::vector<int> marker_ids;detector.detectBoard(image,corners,ids,markers,marker_ids);
    if(image.cols<=0||image.rows<=0||image.cols>kMaxDimension||image.rows>kMaxDimension){v.reject="decoded_dimensions";continue;}if(corners.size()<16||corners.size()!=ids.size()){v.reject="insufficient_charuco";continue;}
    std::set<int> unique;double lo_x=std::numeric_limits<double>::infinity(),lo_y=lo_x,hi_x=-lo_x,hi_y=-lo_x;
    for(size_t i=0;i<ids.size();++i){cv::Point3d p=object_point(ids[i]);if(!finite(p.x)||!unique.insert(ids[i]).second){v.reject="invalid_charuco_id";break;}
      /* WHY: detector and board already originate in float32. CONTRACT: no
       * double observation is cast down here; validation receives its exact
       * one-time promotion of the OpenCV transport value. */
      v.ids.push_back(ids[i]); v.api_object.emplace_back(static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z));
      v.api_image.push_back(corners[i]); v.object.emplace_back(v.api_object.back().x,v.api_object.back().y,v.api_object.back().z);
      v.image.emplace_back(v.api_image.back().x,v.api_image.back().y);
      v.object_conversion_max=std::max(v.object_conversion_max,std::max({std::abs(p.x-v.object.back().x),std::abs(p.y-v.object.back().y),std::abs(p.z-v.object.back().z)}));
      lo_x=std::min(lo_x,v.image.back().x);hi_x=std::max(hi_x,v.image.back().x);lo_y=std::min(lo_y,v.image.back().y);hi_y=std::max(hi_y,v.image.back().y);}
    if(!v.reject.empty()) continue;
    v.width=image.cols; v.height=image.rows;
    v.occupancy=std::min((hi_x-lo_x)/static_cast<double>(image.cols),(hi_y-lo_y)/static_cast<double>(image.rows));
    if(v.occupancy<.20||v.occupancy>.80){v.reject="occupancy";v.ids.clear();v.api_object.clear();v.api_image.clear();v.object.clear();v.image.clear();continue;}
    int physical_quadrants[4]{}; for(const cv::Point3d& p:v.object) { const int q=(p.x>=.135?1:0)+(p.y>=.105?2:0); physical_quadrants[q]=1; }
    if(physical_quadrants[0]+physical_quadrants[1]+physical_quadrants[2]+physical_quadrants[3]<3){v.reject="target_physical_quadrants";continue;}
    const int x0=std::max(0,static_cast<int>(std::floor(lo_x))), x1=std::min(image.cols,static_cast<int>(std::ceil(hi_x))+1);
    const int y0=std::max(0,static_cast<int>(std::floor(lo_y))), y1=std::min(image.rows,static_cast<int>(std::ceil(hi_y))+1);
    size_t clipped=0,total=0;for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){const unsigned char z=image.at<unsigned char>(y,x);clipped+=(z==0||z==255);++total;}
    const double measured_clipping=total?static_cast<double>(clipped)/static_cast<double>(total):1.0;
    if(measured_clipping>.01||v.clipping_fraction>.01){v.reject="clipping";continue;}
    if(v.pre_solve_corner_rms>.25){v.reject="pre_solve_corner_rms";continue;}
    if(v.coordinate_width!=image.cols||v.coordinate_height!=image.rows||v.coordinate_comparison_count<20||v.coordinate_coverage!=0x1ffU||v.coordinate_dx_max>.01||v.coordinate_dy_max>.01){v.reject="coordinate_equivalence";continue;}
    v.clipping_fraction=measured_clipping; v.coordinate_ok=true; v.accepted=true;
  }return true;
}
cv::Point2d project(const Params&p,const cv::Vec3d&r,const cv::Vec3d&t,const cv::Point3d&o){cv::Matx33d R;cv::Rodrigues(r,R);cv::Vec3d q=R*cv::Vec3d(o.x,o.y,o.z)+t;double x=q[0]/q[2],y=q[1]/q[2],r2=x*x+y*y,rad=1+p.k1*r2+p.k2*r2*r2;double xd=x*rad+2*p.p1*x*y+p.p2*(r2+2*x*x),yd=y*rad+p.p1*(r2+2*y*y)+2*p.p2*x*y;return {p.fx*xd+p.cx,p.fy*yd+p.cy};}
bool solve(const std::vector<View>& views,const std::vector<size_t>& indices,cv::Size size,Solve*out,std::string* why){if(indices.empty())return false;
  std::vector<std::vector<cv::Point3f>> op;std::vector<std::vector<cv::Point2f>> ip;
  for(size_t n:indices){if(views[n].api_object.size()!=views[n].object.size()||views[n].api_image.size()!=views[n].image.size()){*why="invalid observation transport";return false;}op.push_back(views[n].api_object);ip.push_back(views[n].api_image);}
  cv::Mat K=cv::Mat::eye(3,3,CV_64F),D=cv::Mat::zeros(5,1,CV_64F),si,se,pve;std::vector<cv::Mat> rv,tv;
  try{out->rms=cv::calibrateCamera(op,ip,size,K,D,rv,tv,si,se,pve,cv::CALIB_FIX_K3,cv::TermCriteria(cv::TermCriteria::COUNT|cv::TermCriteria::EPS,500,DBL_EPSILON));}catch(const cv::Exception&e){*why=e.what();return false;}
  out->p={K.at<double>(0,0),K.at<double>(1,1),K.at<double>(0,2),K.at<double>(1,2),D.at<double>(0),D.at<double>(1),D.at<double>(2),D.at<double>(3)};
  if(!finite(out->p.fx)||out->p.fx<=0||!finite(out->p.fy)||out->p.fy<=0||D.at<double>(4)!=0.0){*why="invalid solve";return false;}out->rvec.clear();out->tvec.clear();for(size_t i=0;i<rv.size();++i){out->rvec.emplace_back(rv[i].at<double>(0),rv[i].at<double>(1),rv[i].at<double>(2));out->tvec.emplace_back(tv[i].at<double>(0),tv[i].at<double>(1),tv[i].at<double>(2));}return true;}
bool classify(std::vector<View>* views,const Solve&s,std::string* why){size_t solved=0;const double one_third=1.0/3.0,two_thirds=2.0/3.0;
  for(View&v:*views){if(!v.accepted)continue;if(solved>=s.rvec.size()||v.image.empty()||v.width<=0||v.height<=0){*why="invalid frame region input";return false;}
    cv::Matx33d R;cv::Rodrigues(s.rvec[solved],R);v.angle=std::acos(std::min(1.0,std::abs(R(2,2))))*180.0/CV_PI;if(v.measured_distance>0)v.distance=v.measured_distance;if(v.declared_distance_band>=0)v.distance_band=v.declared_distance_band;v.angle_class=v.angle<20||v.angle>60?-1:(v.angle<=35?0:(v.angle<=50?1:2));
    if(v.ids.size()!=v.image.size()){*why="target center IDs";return false;}
    std::vector<std::pair<int,cv::Point2d>> ordered;ordered.reserve(v.ids.size());
    for(size_t i=0;i<v.ids.size();++i){if(!finite(v.image[i].x)||!finite(v.image[i].y)){*why="nonfinite target center";return false;}ordered.emplace_back(v.ids[i],v.image[i]);}
    std::sort(ordered.begin(),ordered.end(),[](const auto&a,const auto&b){return a.first<b.first;});
    for(size_t i=1;i<ordered.size();++i)if(ordered[i-1].first==ordered[i].first){*why="duplicate target center ID";return false;}
    double sx=0,sy=0;for(const auto&p:ordered){sx+=p.second.x;sy+=p.second.y;}
    const double u=(sx/static_cast<double>(v.image.size()))/static_cast<double>(v.width),vnorm=(sy/static_cast<double>(v.image.size()))/static_cast<double>(v.height);
    if(!finite(u)||!finite(vnorm)||u<0||u>=1||vnorm<0||vnorm>=1){*why="target center outside image";return false;}
    if(u>=one_third&&u<=two_thirds&&vnorm>=one_third&&vnorm<=two_thirds)v.quadrant=4;
    else if(u<.5) v.quadrant=vnorm<.5?0:2;
    else v.quadrant=vnorm<.5?1:3;
    ++solved;}
  return true;}
bool validate_and_split(std::vector<View>*views,std::vector<size_t>*fit,std::vector<size_t>*hold,std::string*why){size_t corners=0,quad[5]={},dist[3]={},angle[3]={},inclined=0;double min_distance=std::numeric_limits<double>::infinity(),max_distance=0;for(const View&v:*views)if(v.accepted){corners+=v.image.size();min_distance=std::min(min_distance,v.distance);max_distance=std::max(max_distance,v.distance);if(v.quadrant<5)++quad[v.quadrant];if(v.distance_band<3)++dist[v.distance_band];if(v.angle_class>=0){++angle[v.angle_class];++inclined;}}
  size_t accepted=0;for(auto&v:*views)if(v.accepted)++accepted;if(accepted<40||corners<1600||quad[0]<6||quad[1]<6||quad[2]<6||quad[3]<6||quad[4]<8||dist[0]<8||dist[1]<8||dist[2]<8||inclined<24||angle[0]<6||angle[1]<6||angle[2]<6||!finite(min_distance)||min_distance<=0||max_distance/min_distance<1.5){*why="Science v1 support";return false;}
  std::vector<size_t> order;for(size_t i=0;i<views->size();++i)if((*views)[i].accepted)order.push_back(i);std::sort(order.begin(),order.end(),[&](size_t a,size_t b){const View&A=(*views)[a],&B=(*views)[b];return std::tie(A.quadrant,A.distance_band,A.angle_class,A.hash)<std::tie(B.quadrant,B.distance_band,B.angle_class,B.hash);});
  int lq=-1,ld=-1,la=-2;size_t rank=0;for(size_t i:order){View&v=(*views)[i];if(v.quadrant!=lq||v.distance_band!=ld||v.angle_class!=la){lq=v.quadrant;ld=v.distance_band;la=v.angle_class;rank=0;}v.holdout=(rank%5)==4;(v.holdout?hold:fit)->push_back(i);++rank;}size_t fq[4]={};for(size_t i:*fit)if((*views)[i].quadrant<4)++fq[(*views)[i].quadrant];if(fit->size()<32||hold->size()<8||fq[0]<4||fq[1]<4||fq[2]<4||fq[3]<4){*why="Science v1 holdout";return false;}return true;}

/* WHY: OpenCV's RMS is diagnostic only. CONTRACT: residual evidence uses the
 * frozen Lardon3D pinhole model and promoted API observations in binary64. */
bool residuals(std::vector<View>* views,const std::vector<size_t>& indices,
               const Solve& s,double* rms,double* maximum,size_t* high,size_t* count,
               std::string* why) {
  long double sum=0; size_t n=0; *maximum=0; *high=0;
  if(s.rvec.size()!=indices.size()||s.tvec.size()!=indices.size()){*why="pose count";return false;}
  for(size_t j=0;j<indices.size();++j){View& v=(*views)[indices[j]];v.residuals.clear();v.rmse=0;v.max_residual=0;
    for(size_t i=0;i<v.object.size();++i){const cv::Point2d q=project(s.p,s.rvec[j],s.tvec[j],v.object[i]);const cv::Point2d e=q-v.image[i];
      if(!finite(e.x)||!finite(e.y)){*why="nonfinite residual";return false;}const double d=cv::norm(e);v.residuals.push_back(e);v.rmse+=d*d;v.max_residual=std::max(v.max_residual,d);*maximum=std::max(*maximum,d);if(d>1.0)++*high;sum+=static_cast<long double>(d)*d;++n;}
    if(v.object.empty()){*why="empty view";return false;}v.rmse=std::sqrt(v.rmse/static_cast<double>(v.object.size()));}
  if(n==0){*why="no residuals";return false;}*count=n;*rms=std::sqrt(static_cast<double>(sum/static_cast<long double>(n)));return true;
}
bool exact_params(const Solve&a,const Solve&b) {
  const double x[]={a.p.fx,a.p.fy,a.p.cx,a.p.cy,a.p.k1,a.p.k2,a.p.p1,a.p.p2};
  const double y[]={b.p.fx,b.p.fy,b.p.cx,b.p.cy,b.p.k1,b.p.k2,b.p.p1,b.p.p2};
  return std::memcmp(x,y,sizeof(x))==0;
}
bool exact_solve(const Solve& a, const Solve& b) {
  if (!exact_params(a,b) || std::memcmp(&a.rms,&b.rms,sizeof(double)) != 0 ||
      a.rvec.size()!=b.rvec.size() || a.tvec.size()!=b.tvec.size()) return false;
  for(size_t i=0;i<a.rvec.size();++i) if(std::memcmp(a.rvec[i].val,b.rvec[i].val,sizeof(a.rvec[i].val))!=0 ||
      std::memcmp(a.tvec[i].val,b.tvec[i].val,sizeof(a.tvec[i].val))!=0) return false;
  return true;
}
std::string number(double x) { std::ostringstream o; o.imbue(std::locale::classic()); o<<std::hexfloat<<x; return o.str(); }
/* WHY: evidence must be inspectable before a future importer sees it.
 * CONTRACT: these JSON documents are canonically ordered and atomically
 * published as one new sidecar directory; an existing bundle is immutable. */
bool write_bundle(const std::string& session, const SessionEvidence& e, const std::vector<View>& views,
                  const Solve full[3], const Solve& fit, double rms, double maximum, size_t high,
                  size_t count, double hold_rms, double hold_max, double delta, unsigned flags,
                  const std::array<unsigned char,32>& optical, std::string* why) {
  const std::filesystem::path final = session + ".bundle";
  const std::filesystem::path temp = session + ".bundle.tmp";
  std::error_code ec; if(std::filesystem::exists(final,ec)||ec||std::filesystem::exists(temp,ec)){*why="evidence bundle already exists";return false;}
  if(!std::filesystem::create_directory(temp,ec)||ec){*why="cannot create evidence bundle";return false;}
  auto fail=[&](const std::string& x){std::filesystem::remove_all(temp,ec);*why=x;return false;};
  std::ofstream d(temp/"detection.json",std::ios::binary); if(!d)return fail("detection output");
  d<<"{\n\"format\":\"L3DCAL_DETECTION_V1\",\n\"decoder\":\""<<e.decoder<<"\",\n\"decoder_version\":\""<<e.decoder_version<<"\",\n\"views\":[\n";
  for(size_t i=0;i<views.size();++i){const View&v=views[i];d<<"{\"source_sha256\":\""<<hex(v.hash)<<"\",\"orientation\":"<<v.orientation<<",\"oriented_width\":"<<v.width<<",\"oriented_height\":"<<v.height<<",\"decision\":\""<<(v.accepted?"accepted":"rejected")<<"\",\"reason\":\""<<(v.reject.empty()?"-":v.reject)<<"\",\"pre_solve_corner_rms_px\":\""<<number(v.pre_solve_corner_rms)<<"\",\"clipping_fraction\":\""<<number(v.clipping_fraction)<<"\",\"physical_target_quadrants\":";
    int q[4]{};for(const auto&p:v.object)q[(p.x>=.135?1:0)+(p.y>=.105?2:0)]=1;d<<(q[0]+q[1]+q[2]+q[3])<<",\"coordinate_equivalence\":{\"comparison_points\":"<<v.coordinate_comparison_count<<",\"max_abs_dx_px\":\""<<number(v.coordinate_dx_max)<<"\",\"max_abs_dy_px\":\""<<number(v.coordinate_dy_max)<<"\",\"pass\":"<<(v.coordinate_ok?"true":"false")<<"},\"corners\":[";
    for(size_t j=0;j<v.ids.size();++j){if(j)d<<',';d<<"{\"id\":"<<v.ids[j]<<",\"x\":\""<<number(v.image[j].x)<<"\",\"y\":\""<<number(v.image[j].y)<<"\"}";}d<<"]}"<<(i+1==views.size()?"\n":" ,\n");}
  d<<"]\n}\n";d.close();if(!d)return fail("detection write");
  std::ofstream s(temp/"solve.json",std::ios::binary);if(!s)return fail("solve output");
  s<<"{\n\"format\":\"L3DCAL_SOLVE_V1\",\n\"runs\":[\n";for(int run=0;run<3;++run){const Solve&z=full[run];s<<"{\"run\":"<<run<<",\"params\":[\""<<number(z.p.fx)<<"\",\""<<number(z.p.fy)<<"\",\""<<number(z.p.cx)<<"\",\""<<number(z.p.cy)<<"\",\""<<number(z.p.k1)<<"\",\""<<number(z.p.k2)<<"\",\""<<number(z.p.p1)<<"\",\""<<number(z.p.p2)<<"\"],\"opencv_rms_px\":\""<<number(z.rms)<<"\",\"poses\":[";for(size_t i=0;i<z.rvec.size();++i){if(i)s<<',';s<<"{\"rvec\":[\""<<number(z.rvec[i][0])<<"\",\""<<number(z.rvec[i][1])<<"\",\""<<number(z.rvec[i][2])<<"\"],\"tvec_m\":[\""<<number(z.tvec[i][0])<<"\",\""<<number(z.tvec[i][1])<<"\",\""<<number(z.tvec[i][2])<<"\"]}";}s<<"]}"<<(run==2?"\n":" ,\n");}s<<"],\n\"fit_params\":[\""<<number(fit.p.fx)<<"\",\""<<number(fit.p.fy)<<"\",\""<<number(fit.p.cx)<<"\",\""<<number(fit.p.cy)<<"\",\""<<number(fit.p.k1)<<"\",\""<<number(fit.p.k2)<<"\",\""<<number(fit.p.p1)<<"\",\""<<number(fit.p.p2)<<"\"]\n}\n";s.close();if(!s)return fail("solve write");
  std::ofstream f(temp/"evidence.json",std::ios::binary);if(!f)return fail("final evidence output");
  f<<"{\n\"format\":\"L3DCAL_EVIDENCE_BUNDLE_V1\",\n\"target\":{\"id\":\""<<e.target_id<<"\",\"generator_sha256\":\""<<hex(e.generator_hash)<<"\",\"instrument\":\""<<e.instrument<<"\",\"resolution_mm\":\""<<number(e.resolution_mm)<<"\",\"planarity_evidence_sha256\":\""<<hex(e.planarity_hash)<<"\"},\n\"optical_sha256\":\""<<hex(optical)<<"\",\n\"optical_state\":\""<<e.optical_state_fields<<"\",\n\"validation_flags\":\"0x"<<std::hex<<flags<<std::dec<<"\",\n\"global_rmse_px\":\""<<number(rms)<<"\",\"maximum_residual_px\":\""<<number(maximum)<<"\",\"high_residual_fraction\":\""<<number(static_cast<double>(high)/static_cast<double>(count))<<"\",\n\"holdout\":{\"rmse_px\":\""<<number(hold_rms)<<"\",\"maximum_px\":\""<<number(hold_max)<<"\"},\n\"maximum_parameter_delta_px\":\""<<number(delta)<<"\",\n\"deterministic_full_solve_equality\":"<<((flags&4)?"true":"false")<<",\n\"residuals\":[";
  bool first=true;for(const View&v:views)if(v.accepted)for(size_t i=0;i<v.residuals.size();++i){if(!first)f<<',';first=false;f<<"{\"source_sha256\":\""<<hex(v.hash)<<"\",\"corner_id\":"<<v.ids[i]<<",\"dx_px\":\""<<number(v.residuals[i].x)<<"\",\"dy_px\":\""<<number(v.residuals[i].y)<<"\",\"rmse_px\":\""<<number(v.rmse)<<"\"}";}f<<"]\n}\n";f.close();if(!f)return fail("final evidence write");
  std::filesystem::rename(temp,final,ec);if(ec)return fail("evidence publication");return true;
}
void report(const std::vector<View>&v,const Solve&s,double independent_rms,
            double independent_max,size_t high,double delta,
            const std::array<unsigned char,32>& optical) {
  std::cout.imbue(std::locale::classic());std::cout<<std::hexfloat;
  struct utsname u{};const bool uname_ok=uname(&u)==0;
  std::cout<<"L3DCAL_EVIDENCE_V1\nsolver opencv\nopencv_version "<<CV_VERSION<<"\nopencv_build_sha256 "<<hex(text_hash(cv::getBuildInformation()))<<"\nthreads 1\narchitecture "<<(uname_ok?u.machine:"unknown")<<"\noptical_sha256 "<<hex(optical)<<"\n";
  std::cout<<"conversion_path Point2f_Point3f_once_then_exact_Point2d_Point3d\n";
  std::cout<<"params "<<s.p.fx<<' '<<s.p.fy<<' '<<s.p.cx<<' '<<s.p.cy<<' '<<s.p.k1<<' '<<s.p.k2<<' '<<s.p.p1<<' '<<s.p.p2<<"\n";
  std::cout<<"opencv_rms "<<s.rms<<"\nindependent_rms "<<independent_rms<<"\nindependent_max "<<independent_max<<"\nhigh_residual_count "<<high<<"\nmaximum_parameter_delta "<<delta<<"\n";
  for(const View&x:v){std::cout<<"view "<<hex(x.hash)<<' '<<(x.accepted?"accepted":"rejected")<<' '<<(x.reject.empty()?"-":x.reject)<<' '<<x.image_conversion_max<<' '<<x.object_conversion_max<<"\n";}
}
bool run_session(const std::string& session) {
  std::vector<View> v;std::array<unsigned char,32> optical{};SessionEvidence evidence;std::string why;
  if(!read_session(session,&v,&optical,&evidence,&why)||!detect(&v,&why)){std::cerr<<"FAIL "<<why<<'\n';return false;}
  std::vector<size_t> all;cv::Size size;for(size_t i=0;i<v.size();++i)if(v[i].accepted){all.push_back(i);if(size.empty())size={v[i].width,v[i].height};else if(size.width!=v[i].width||size.height!=v[i].height){std::cerr<<"FAIL dimensions\n";return false;}}
  Solve initial;if(!solve(v,all,size,&initial,&why)||!classify(&v,initial,&why)){std::cerr<<"FAIL "<<why<<'\n';return false;}std::vector<size_t>fit,hold;if(!validate_and_split(&v,&fit,&hold,&why)){std::cerr<<"FAIL "<<why<<'\n';return false;}
  Solve full[3],fit_s;if(!solve(v,all,size,&full[0],&why)||!solve(v,all,size,&full[1],&why)||!solve(v,all,size,&full[2],&why)||!exact_solve(full[0],full[1])||!exact_solve(full[0],full[2])||!solve(v,fit,size,&fit_s,&why)){std::cerr<<"FAIL deterministic_or_solve "<<why<<'\n';return false;}
  double hr=0,hm=0;size_t hh=0,hold_count=0;if(!residuals(&v,hold,fit_s,&hr,&hm,&hh,&hold_count,&why)||hold_count==0){std::cerr<<"FAIL "<<why<<'\n';return false;}
  double rms=0,mx=0;size_t high=0,residual_count=0;if(!residuals(&v,all,full[0],&rms,&mx,&high,&residual_count,&why)){std::cerr<<"FAIL "<<why<<'\n';return false;}
  auto eval=[](const Params&p,cv::Point2d q){double r2=q.x*q.x+q.y*q.y,rad=1+p.k1*r2+p.k2*r2*r2;return cv::Point2d(p.fx*(q.x*rad+2*p.p1*q.x*q.y+p.p2*(r2+2*q.x*q.x))+p.cx,p.fy*(q.y*rad+p.p1*(r2+2*q.y*q.y)+2*p.p2*q.x*q.y)+p.cy);};double delta=0;for(auto q:std::array<cv::Point2d,5>{{{0,0},{-.7,-.7},{.7,-.7},{-.7,.7},{.7,.7}}})delta=std::max(delta,cv::norm(eval(full[0].p,q)-eval(fit_s.p,q)));
  bool view_rmse=false;for(const View&x:v)if(x.accepted&&x.rmse>.75)view_rmse=true;
  const bool high_ok=static_cast<double>(high)/static_cast<double>(residual_count)<=.01;
  const bool quantitative_ok=rms<=.50&&mx<=1.50&&high_ok&&!view_rmse&&hr<=.75&&hm<=1.50&&delta<=.10;
  if(!quantitative_ok){std::cerr<<"FAIL Science v1 residual thresholds\n";return false;}
  bool coordinate_ok=true;for(const View&x:v)if(x.accepted&&!x.coordinate_ok)coordinate_ok=false;
  /* The flags are predicates over independently retained evidence.  They are
   * deliberately assembled here rather than being a successful-run constant. */
  unsigned flags=0; if(quantitative_ok)flags|=0x01; if(!fit.empty()&&!hold.empty())flags|=0x02;
  if(exact_solve(full[0],full[1])&&exact_solve(full[0],full[2])) flags|=0x04;
  if(coordinate_ok) flags|=0x08;
  if(flags!=0x0f||!write_bundle(session,evidence,v,full,fit_s,rms,mx,high,residual_count,hr,hm,delta,flags,optical,&why)){std::cerr<<"FAIL "<<(flags!=0x0f?"validation flags":why)<<'\n';return false;}
  report(v,full[0],rms,mx,high,delta,optical);return true;
}

bool synthetic_test() {
  /* Synthetic only: known binary64 camera/poses, never a physical calibration. */
  Params truth{1400,1360,1000,750,-.08,.015,.001,-.0008}; std::vector<View> v(60);
  std::array<int,10> qs{{0,0,1,1,2,2,3,3,4,4}};
  std::array<int,10> ds{{0,1,2,0,1,2,0,1,2,0}};
  std::array<double,10> as{{25,40,55,25,40,55,25,40,55,25}};
  for(size_t i=0;i<v.size();++i){View&x=v[i];size_t g=i/6;x.accepted=true;x.width=2000;x.height=1500;x.distance_band=ds[g];x.angle=as[g];x.angle_class=as[g]<=35?0:(as[g]<=50?1:2);x.occupancy=.30;
    for(int id=0;id<48;++id){const cv::Point3d o=object_point(id);x.ids.push_back(id);
      /* Synthetic truth starts in binary64 solely to measure the mandated
       * single transport quantization before the qualified OpenCV call. */
      x.api_object.emplace_back(static_cast<float>(o.x),static_cast<float>(o.y),static_cast<float>(o.z));
      x.object.emplace_back(x.api_object.back().x,x.api_object.back().y,x.api_object.back().z);
      x.object_conversion_max=std::max(x.object_conversion_max,std::max({std::abs(o.x-x.object.back().x),std::abs(o.y-x.object.back().y),std::abs(o.z-x.object.back().z)}));}
    const double tx=(qs[g]==0||qs[g]==2)?-.25:((qs[g]==1||qs[g]==3)?-.02:-.135);
    const double ty=(qs[g]==0||qs[g]==1)?-.17:((qs[g]==2||qs[g]==3)?0.0:-.105);
    double a=as[g]*CV_PI/180.;cv::Vec3d r(a,0,0),t(tx,ty,ds[g]==0?.30:(ds[g]==1?.38:.48));x.distance=cv::norm(t);
    for(const auto&o:x.object){const cv::Point2d d=project(truth,r,t,o);x.api_image.emplace_back(static_cast<float>(d.x),static_cast<float>(d.y));x.image.emplace_back(x.api_image.back().x,x.api_image.back().y);x.image_conversion_max=std::max(x.image_conversion_max,std::max(std::abs(d.x-x.image.back().x),std::abs(d.y-x.image.back().y)));}
  }
  std::vector<size_t> all;for(size_t i=0;i<v.size();++i)all.push_back(i);Solve a,b,c;std::string why;
  if(!solve(v,all,{2000,1500},&a,&why)||!solve(v,all,{2000,1500},&b,&why)||!solve(v,all,{2000,1500},&c,&why)){std::cerr<<why<<'\n';return false;}
  if(!exact_params(a,b)||!exact_params(a,c)){std::cerr<<"repeat\n";return false;}if(std::abs(a.p.fx-truth.fx)>.05||std::abs(a.p.fy-truth.fy)>.05||std::abs(a.p.cx-truth.cx)>.05||std::abs(a.p.cy-truth.cy)>.05){std::cerr<<std::setprecision(17)<<a.p.fx<<' '<<a.p.fy<<' '<<a.p.cx<<' '<<a.p.cy<<'\n';return false;}
  if(!classify(&v,a,&why)){std::cerr<<why<<'\n';return false;}
  std::vector<size_t> fit,hold;if(!validate_and_split(&v,&fit,&hold,&why)){std::cerr<<why<<'\n';return false;}
  Solve f;if(!solve(v,fit,{2000,1500},&f,&why))return false;
  double delta=0;for(auto ray:std::array<cv::Point2d,5>{{{0,0},{-.7,-.7},{.7,-.7},{-.7,.7},{.7,.7}}}){auto eval=[](const Params&p,cv::Point2d q){double r2=q.x*q.x+q.y*q.y,rad=1+p.k1*r2+p.k2*r2*r2;return cv::Point2d(p.fx*(q.x*rad+2*p.p1*q.x*q.y+p.p2*(r2+2*q.x*q.x))+p.cx,p.fy*(q.y*rad+p.p1*(r2+2*q.y*q.y)+2*p.p2*q.x*q.y)+p.cy);};delta=std::max(delta,cv::norm(eval(a.p,ray)-eval(f.p,ray)));}
  double iq=0,oq=0;for(const auto&x:v){iq=std::max(iq,x.image_conversion_max);oq=std::max(oq,x.object_conversion_max);}
  double rr=0,rm=0;size_t rh=0,rc=0;if(!residuals(&v,all,a,&rr,&rm,&rh,&rc,&why))return false;
  SessionEvidence evidence; evidence.target=true;evidence.measurement=true;evidence.planarity=true;evidence.planarity_pass=1;evidence.decoder_set=true;evidence.optical_state=true;evidence.target_id="synthetic";evidence.instrument="synthetic";evidence.decoder="synthetic_decoder";evidence.decoder_version="1";evidence.optical_state_fields="synthetic_only";evidence.resolution_mm=.1;evidence.measurements.fill(30.0);
  Solve runs[3]={a,b,c};std::array<unsigned char,32> optical{};std::string bundle_why;
  const std::filesystem::path left=std::filesystem::temp_directory_path()/"l3dcal_byte_identity_left";
  const std::filesystem::path right=std::filesystem::temp_directory_path()/"l3dcal_byte_identity_right";std::error_code ec;std::filesystem::remove_all(left.string()+".bundle",ec);std::filesystem::remove_all(right.string()+".bundle",ec);
  const bool written=write_bundle(left.string(),evidence,v,runs,f,rr,rm,rh,rc,rr,rm,delta,0x0f,optical,&bundle_why)&&write_bundle(right.string(),evidence,v,runs,f,rr,rm,rh,rc,rr,rm,delta,0x0f,optical,&bundle_why);
  auto identical=[&](const char* name){std::ifstream x(left.string()+".bundle/"+name,std::ios::binary),y(right.string()+".bundle/"+name,std::ios::binary);std::ostringstream xs,ys;xs<<x.rdbuf();ys<<y.rdbuf();return x&&y&&xs.str()==ys.str();};
  const bool bytes=written&&identical("detection.json")&&identical("solve.json")&&identical("evidence.json");std::filesystem::remove_all(left.string()+".bundle",ec);std::filesystem::remove_all(right.string()+".bundle",ec);
  std::cout<<std::hexfloat<<"synthetic_image_conversion_max "<<iq<<"\nsynthetic_object_conversion_max_m "<<oq<<"\n";
  std::cout<<"synthetic_opencv_rms "<<a.rms<<"\nsynthetic_parameter_delta "<<delta<<"\n"<<std::defaultfloat;
  if (!bytes) std::cerr << "synthetic byte identity failure: " << bundle_why << '\n';
  return iq<.01&&delta<=.10&&bytes;
}
bool manifest_negative_tests() {
  const std::string h(64, '0');
  const std::filesystem::path p=std::filesystem::temp_directory_path()/"l3dcal_manifest_negative.l3dcal";
  auto make=[&](const std::string& replacement) {
    std::ostringstream x; x.imbue(std::locale::classic());
    x<<"L3DCAL_SESSION_V1\n"
     <<"target board "<<h<<" DICT_5X5_100 9 7 30 21\n"
     <<"measurement caliper 0.1 30 30 30 30 30 30 30 30 30 30\n"
     <<"planarity PASS "<<h<<"\n"
     <<"decoder qualified_decoder 1\n"
     <<"optical_state "<<h<<" body_objective_zoom_focus_stabilization_format_pipeline\n"
     <<"image /nonexistent "<<h<<" 0\n"
     <<"pre_solve "<<h<<" 0.25\n"
     <<"clipping "<<h<<" 0.01\n"
     <<"coordinate "<<h<<" qualified_decoder 1 0 100 100 20 0 0\n";
    const std::array<const char*,9> coverage{{"center","top","right","bottom","left","top_left","top_right","bottom_left","bottom_right"}};
    for(int i=0;i<20;++i)x<<"coordinate_point "<<h<<' '<<coverage[static_cast<size_t>(i)%coverage.size()]<<' '<<i<<" 0 "<<i<<" 0\n";
    x<<"distance "<<h<<" 0.4 1\n";
    std::string text=x.str(); const size_t at=text.find("@@");
    if(replacement.empty()) return text;
    if(at!=std::string::npos) text.replace(at,2,replacement);
    return text;
  };
  auto parse=[&](std::string text)->bool { std::ofstream o(p,std::ios::binary);o<<text;o.close();std::vector<View>v;std::array<unsigned char,32>op{};SessionEvidence e;std::string why;return read_session(p.string(),&v,&op,&e,&why); };
  const std::string good=make("");
  bool ok=parse(good); if(!ok)std::cerr<<"negative good\n";
  auto altered=[&](const std::string& from,const std::string& to){std::string t=good;const size_t n=t.find(from);if(n==std::string::npos)return false;t.replace(n,from.size(),to);return !parse(t);};
  const bool n1=altered("target board", "target_bad board"),n2=altered("planarity PASS", "planarity FAIL"),n3=altered(" 20 0 0\ncoordinate_point", " 19 0 0\ncoordinate_point"),n4=altered("qualified_decoder 1 0 100", "qualified_decoder 1 90 100");ok=ok&&n1&&n2&&n3&&n4;
  std::error_code ec;std::filesystem::remove(p,ec);return ok;
}
} // namespace

int main(int argc,char**argv){std::ios::sync_with_stdio(false);std::cout.imbue(std::locale::classic());cv::setNumThreads(1);cv::setRNGSeed(kSeed);
  if(argc==2&&std::string(argv[1])=="--self-test"){bool ok=synthetic_test()&&manifest_negative_tests();std::cout<<(ok?"SELF_TEST_PASS\n":"SELF_TEST_FAIL\n");return ok?0:1;}
  if(argc==3&&std::string(argv[1])=="--session"){return run_session(argv[2])?0:1;}
  std::cerr<<"usage: calibration_evidence_solver --self-test | --session SESSION\n";return 2;}

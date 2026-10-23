#include "tipe/local_language_model.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace tipe {
namespace {
std::vector<uint32_t> characters(std::string_view text) {
  std::vector<uint32_t> out;
  if (text.size() > 256) return out;
  for (size_t i=0; i<text.size();) {
    const auto first=static_cast<unsigned char>(text[i++]);
    uint32_t code=first; size_t tail=0; uint32_t minimum=0;
    if (first<0x80) {}
    else if ((first&0xe0)==0xc0) {code=first&31;tail=1;minimum=0x80;}
    else if ((first&0xf0)==0xe0) {code=first&15;tail=2;minimum=0x800;}
    else if ((first&0xf8)==0xf0) {code=first&7;tail=3;minimum=0x10000;}
    else return {};
    if (i+tail>text.size()) return {};
    while(tail--) {
      const auto c=static_cast<unsigned char>(text[i++]);
      if((c&0xc0)!=0x80) return {};
      code=(code<<6)|(c&63);
    }
    if(code<minimum || code>0x10ffff || (code>=0xd800 && code<=0xdfff)) return {};
    out.push_back(code); if(out.size()>64) return {};
  }
  return out;
}
constexpr uint32_t boundary=0x110000;
uint64_t pair_key(uint32_t left,uint32_t right) {return (uint64_t(left)<<32)|right;}
}
void LocalLanguageModel::observe(std::string_view text,std::string_view context,uint64_t support) {
  if(support<3 || context.size()>512) return;
  auto chars=characters(text); if(chars.empty()) return;
  // Raw English habits are stored separately and must not train Chinese LM.
  if(std::none_of(chars.begin(),chars.end(),[](auto c){return c>=0x3400 && c<=0x3134f;})) return;
  const std::string key(context);
  if(!contexts_.contains(key) && contexts_.size()>=16) return;
  auto &model=contexts_[key];
  const uint32_t weight=static_cast<uint32_t>(std::min<uint64_t>(support,32));
  uint32_t previous=boundary;
  chars.push_back(boundary);
  for(auto c:chars) {
    const auto key=pair_key(previous,c);
    if(!model.pairs.contains(key) && transitions_>=4096) {previous=c;continue;}
    auto [it,inserted]=model.pairs.try_emplace(key,0);
    if(inserted) ++transitions_;
    const auto added=std::min<uint32_t>(weight,1000000-it->second);
    it->second+=added;model.totals[previous]+=added;previous=c;
  }
}
double LocalLanguageModel::score(std::string_view text,std::string_view context) const {
  const auto found=contexts_.find(std::string(context));
  if(found==contexts_.end()) return 0;
  auto chars=characters(text); if(chars.empty()) return 0;
  const auto &model=found->second;
  const double vocabulary=static_cast<double>(model.totals.size()+1);
  double score=0;uint32_t previous=boundary;chars.push_back(boundary);
  for(auto c:chars) {
    const auto pair=model.pairs.find(pair_key(previous,c));
    const auto total=model.totals.find(previous);
    const double count=pair==model.pairs.end()?0:pair->second;
    const double sum=total==model.totals.end()?0:total->second;
    score+=std::log((count+0.25)/(sum+0.25*vocabulary));previous=c;
  }
  return score/chars.size();
}
}

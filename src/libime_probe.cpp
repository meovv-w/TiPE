#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyinime.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/core/userlanguagemodel.h>
#include <iostream>
#include <memory>
int main(int argc, char **argv){
  auto d=std::make_unique<libime::PinyinDictionary>();
  d->load(0,"/usr/share/libime/sc.dict",libime::PinyinDictFormat::Binary);
  auto lm=std::make_unique<libime::UserLanguageModel>("/usr/lib64/libime/zh_CN.lm");
  libime::PinyinIME ime(std::move(d),std::move(lm)); ime.setNBest(10);
  libime::PinyinContext c(&ime); c.type(argc>1 ? argv[1] : "nihao");
  std::cout<<"preedit="<<c.preedit()<<" candidates="<<c.candidates().size()<<"\n";
  for(size_t i=0;i<c.candidates().size() && i<10;i++) {const auto&r=c.candidates()[i];std::cout<<i<<":"<<r.toString()<<" py="<<c.candidateFullPinyin(i)<<" score="<<r.score(); if(!r.sentence().empty())std::cout<<" consumed="<<r.sentence().back()->to()->index();std::cout<<"\n";}
  c.setCursor(std::min<size_t>(2, c.size())); std::cout<<"toCursor="<<c.candidatesToCursor().size()<<"\n"; for(size_t i=0;i<c.candidatesToCursor().size() && i<5;i++) std::cout<<"tc"<<i<<":"<<c.candidatesToCursor()[i].toString()<<"\n";
}

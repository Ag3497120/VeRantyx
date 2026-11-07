#pragma once
// Optional local loading renderer; unavailable assets never stop the game.
struct LoadingBridge {
 void* lib=nullptr;void* context=nullptr;SharedBridge image;uint32_t last=0;uint32_t width=2048,height=1024;
 using Create=void*(*)(const char*,const char*,const char*);using Destroy=void(*)(void*);
 using Frame=uint32_t(*)(void*,const float*,const float*,float,double,double,uint8_t*,size_t,float*,double*);
 Frame frame=nullptr;Destroy destroy=nullptr;
 LoadingBridge(uint32_t eyeWidth=1024,uint32_t eyeHeight=1024){
  const char* dll=getenv("VERANTYX_LOADING_LIBRARY");const char* state=getenv("VERANTYX_NATIVE_LOADING_FILE");const char* game=getenv("VERANTYX_NATIVE_GAME_DIR");const char* stage=getenv("VERANTYX_NATIVE_STAGE_FILE");
  if(!dll||!state||!game||!stage)return;
  lib=dlopen(dll,RTLD_NOW|RTLD_LOCAL);if(!lib){puts("Loading renderer unavailable; game output retained.");return;}
  auto create=(Create)dlsym(lib,"alyx_loading_create");frame=(Frame)dlsym(lib,"alyx_loading_frame");destroy=(Destroy)dlsym(lib,"alyx_loading_destroy");
  if(create&&frame&&destroy)context=create(state,game,stage);
  if(context){
   using Resize=int32_t(*)(void*,int32_t,int32_t);
   auto resize=(Resize)dlsym(lib,"alyx_loading_resize");
   if(eyeWidth<=4096 && eyeHeight<=4096 && resize && resize(context,int32_t(eyeWidth*2),int32_t(eyeHeight))){width=eyeWidth*2;height=eyeHeight;}
   printf("Loading screen resolution: %ux%u per eye\n",width/2,height);
   if(const char* path=getenv("VERANTYX_LOADING_CAPABILITY_FILE")){FILE* f=fopen(path,"w");if(f){fputs("ready\n",f);fclose(f);}}
   puts("Alyx native loading room/map renderer enabled.");
  }
 }
 // Swift's async worker can retain its scene briefly after release. Keep its
 // code image loaded until process exit rather than dlclose under that worker.
 ~LoadingBridge(){if(context&&destroy)destroy(context);}
 bool update(const SharedBridge& game,double now,bool& changed){
  if(!context||!game.originReady)return false;
  vrrt::SharedHands hands{};memcpy(&hands,game.map+vrrt::kHandsOffset,sizeof hands);
  image.pixels.resize(size_t(width)*height*4);
  float ipd=getenv("VERANTYX_IPD")?strtof(getenv("VERANTYX_IPD"),nullptr):0.064f;
  if(!std::isfinite(ipd)||ipd<0.04f||ipd>0.09f)ipd=0.064f;
  uint32_t seq=frame(context,hands.headTransform,hands.leftTangents,ipd,hands.visionTimestamp,now,image.pixels.data(),image.pixels.size(),image.frame.renderHeadTransform,&image.frame.visionTimestamp);
  if(!seq){last=0;return false;}
  changed=seq!=last;last=seq;image.sequence=seq;image.frame.sequenceNumber=seq;image.frame.width=width;image.frame.height=height;image.frame.format=87;
  image.projection.sequence=seq;memcpy(image.projection.tangents,hands.leftTangents,32);
  image.originReady=game.originReady;image.originYaw=game.originYaw;image.originX=game.originX;image.originY=game.originY;image.originZ=game.originZ;
  return true;
 }
};

#include "objectsbucket.h"

#include <Tempest/Log>

#include "pose.h"
#include "sceneglobals.h"
#include "skeleton.h"

#include "graphics/dynamic/painter3d.h"
#include "utils/workers.h"
#include "rendererstorage.h"

using namespace Tempest;

void ObjectsBucket::Item::setObjMatrix(const Tempest::Matrix4x4 &mt) {
  owner->setObjMatrix(id,mt);
  }

void ObjectsBucket::Item::setPose(const Pose &p) {
  owner->setPose(id,p);
  }

void ObjectsBucket::Item::setBounds(const Bounds& bbox) {
  owner->setBounds(id,bbox);
  }

void ObjectsBucket::Item::draw(Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId) const {
  owner->draw(id,p,fId);
  }


void ObjectsBucket::Descriptors::invalidate() {
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i) {
    uboBit[i] = 0;
    for(auto& b:uboBitSh[i])
      b = 0;
    }
  }

void ObjectsBucket::Descriptors::alloc(ObjectsBucket& owner) {
  if(!ubo[0].isEmpty())
    return;

  for(size_t i=0;i<Resources::MaxFramesInFlight;++i) {
    if(owner.pMain!=nullptr)
      ubo[i] = owner.scene.storage.device.uniforms(owner.pMain->layout());
    if(owner.pShadow!=nullptr) {
      for(size_t lay=0;lay<Resources::ShadowLayers;++lay)
        uboSh[i][lay] = owner.scene.storage.device.uniforms(owner.pShadow->layout());
      }
    }

  owner.uboSetCommon(*this);
  }

ObjectsBucket::ObjectsBucket(const Material& mat, const SceneGlobals& scene, Storage& storage, const Type type)
  :scene(scene), storage(storage), mat(mat), shaderType(type), useSharedUbo(type!=Animated) {
  static_assert(sizeof(UboPush)<=128, "UboPush is way too big");

  switch(mat.alpha) {
    case Material::AlphaTest:
      if(shaderType==Animated) {
        pMain   = &scene.storage.pAnimAt;
        pLight  = &scene.storage.pAnimAtLt;
        pShadow = &scene.storage.pAnimAtSh;
        } else {
        pMain   = &scene.storage.pObjectAt;
        pLight  = &scene.storage.pObjectAtLt;
        pShadow = &scene.storage.pObjectAtSh;
        }
      break;
    case Material::Transparent:
      if(shaderType==Animated) {
        pMain   = &scene.storage.pAnimAlpha;
        pLight  = nullptr;
        pShadow = nullptr;
        } else {
        pMain   = &scene.storage.pObjectAlpha;
        //pLight  = &scene.storage.pObjectLt;
        pShadow = &scene.storage.pObjectAtSh;
        }
      break;
    case Material::AdditiveLight: {
      if(shaderType==Animated) {
        pMain   = &scene.storage.pAnimMAdd;
        pLight  = nullptr;
        pShadow = nullptr;
        } else {
        pMain   = &scene.storage.pObjectMAdd;
        pLight  = nullptr;
        pShadow = nullptr;
        }
      break;
      }
    case Material::Multiply:
    case Material::Multiply2:
    case Material::Solid:
      if(shaderType==Animated) {
        pMain   = &scene.storage.pAnim;
        pLight  = &scene.storage.pAnimLt;
        pShadow = &scene.storage.pAnimSh;
        } else {
        pMain   = &scene.storage.pObject;
        pLight  = &scene.storage.pObjectLt;
        pShadow = &scene.storage.pObjectSh;
        }
      break;
    case Material::InvalidAlpha:
    case Material::LastGothic:
    case Material::FirstOpenGothic:
    case Material::Last:
      break;
    }

  if(mat.frames.size()>0)
    useSharedUbo = false;

  textureInShadowPass = (pShadow==&scene.storage.pObjectAtSh || pShadow==&scene.storage.pAnimAtSh);

  for(auto& i:uboMat)
    i = scene.storage.device.ubo<UboMaterial>(nullptr,1);

  if(useSharedUbo) {
    uboShared.invalidate();
    uboShared.alloc(*this);
    }
  }

ObjectsBucket::~ObjectsBucket() {
  }

const Material& ObjectsBucket::material() const {
  return mat;
  }

ObjectsBucket::Object& ObjectsBucket::implAlloc(const VboType type, const Bounds& bounds) {
  Object* v = nullptr;
  if(freeList.size()>0) {
    v = &val[freeList.back()];
    freeList.pop_back();
    } else {
    val.emplace_back();
    v = &val.back();
    index.resize(val.size());
    }

  v->vboType   = type;
  v->vbo       = nullptr;
  v->vboA      = nullptr;
  v->ibo       = nullptr;
  v->bounds    = bounds;
  v->timeShift = uint64_t(-scene.tickCount);

  if(!useSharedUbo) {
    v->ubo.invalidate();
    if(v->ubo.ubo[0].isEmpty())
      v->ubo.alloc(*this);
    }
  return *v;
  }

void ObjectsBucket::uboSetCommon(Descriptors& v) {
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i) {
    auto& t   = *mat.tex;
    auto& ubo = v.ubo[i];

    if(!ubo.isEmpty()) {
      ubo.set(0,t);
      ubo.set(1,*scene.shadowMap,Resources::shadowSampler());
      ubo.set(2,scene.uboGlobalPf[i][0]);
      ubo.set(4,uboMat[i]);
      }

    if(pShadow!=nullptr) {
      for(size_t lay=0;lay<Resources::ShadowLayers;++lay) {
        auto& uboSh = v.uboSh[i][lay];

        if(textureInShadowPass)
          uboSh.set(0,t);
        uboSh.set(2,scene.uboGlobalPf[i][lay]);
        uboSh.set(4,uboMat[i]);
        }
      }
    }
  }

void ObjectsBucket::setupUbo() {
  for(auto& v:val) {
    setupLights(v,true);
    }

  if(useSharedUbo) {
    uboShared.invalidate();
    uboSetCommon(uboShared);
    } else {
    for(auto& i:val) {
      i.ubo.invalidate();
      uboSetCommon(i.ubo);
      }
    }
  }

void ObjectsBucket::invalidateUbo() {
  if(useSharedUbo) {
    uboShared.invalidate();
    } else {
    for(auto& i:val)
      i.ubo.invalidate();
    }
  }

void ObjectsBucket::preFrameUpdate(uint8_t fId) {
  UboMaterial ubo;
  if(mat.texAniMapDirPeriod.x!=0)
    ubo.texAniMapDir.x = float(scene.tickCount%std::abs(mat.texAniMapDirPeriod.x))/float(mat.texAniMapDirPeriod.x);
  if(mat.texAniMapDirPeriod.y!=0)
    ubo.texAniMapDir.y = float(scene.tickCount%std::abs(mat.texAniMapDirPeriod.y))/float(mat.texAniMapDirPeriod.y);

  uboMat[fId].update(&ubo,0,1);
  if(mat.frames.size()>0) {
    //texAnim;
    }
  }

bool ObjectsBucket::groupVisibility(Painter3d& p) {
  if(shaderType!=Static)
    return true;

  if(allBounds.r<=0) {
    Tempest::Vec3 bbox[2] = {};
    bool          fisrt=true;
    for(size_t i=0;i<val.size();++i) {
      if(val[i].ibo==nullptr)
        continue;
      auto& b = val[i].bounds;
      if(fisrt) {
        bbox[0] = b.bboxTr[0];
        bbox[1] = b.bboxTr[1];
        fisrt = false;
        }
      bbox[0].x = std::min(bbox[0].x,b.bboxTr[0].x);
      bbox[0].y = std::min(bbox[0].y,b.bboxTr[0].y);
      bbox[0].z = std::min(bbox[0].z,b.bboxTr[0].z);

      bbox[1].x = std::max(bbox[1].x,b.bboxTr[1].x);
      bbox[1].y = std::max(bbox[1].y,b.bboxTr[1].y);
      bbox[1].z = std::max(bbox[1].z,b.bboxTr[1].z);
      }
    allBounds.assign(bbox);
    }
  return p.isVisible(allBounds);
  }

void ObjectsBucket::visibilityPass(Painter3d& p) {
  index.clear();
  if(!groupVisibility(p))
    return;

  for(auto& v:val) {
    if(v.vboType==VboType::NoVbo)
      continue;
    if(!p.isVisible(v.bounds) && v.vboType!=VboType::VboMorph)
      continue;
    index.push_back(&v);
    }
  }

size_t ObjectsBucket::alloc(const Tempest::VertexBuffer<Vertex>&  vbo,
                            const Tempest::IndexBuffer<uint32_t>& ibo,
                            const Bounds& bounds) {
  Object* v = &implAlloc(VboType::VboVertex,bounds);
  v->vbo = &vbo;
  v->ibo = &ibo;
  return std::distance(val.data(),v);
  }

size_t ObjectsBucket::alloc(const Tempest::VertexBuffer<VertexA>& vbo,
                            const Tempest::IndexBuffer<uint32_t>& ibo,
                            const Bounds& bounds) {
  Object* v = &implAlloc(VboType::VboVertexA,bounds);
  v->vboA   = &vbo;
  v->ibo    = &ibo;
  v->storageAni = storage.ani.alloc();
  return std::distance(val.data(),v);
  }

size_t ObjectsBucket::alloc(const Tempest::VertexBuffer<ObjectsBucket::Vertex>* vbo[], const Bounds& bounds) {
  Object* v = &implAlloc(VboType::VboMorph,bounds);
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i)
    v->vboM[i] = vbo[i];
  return std::distance(val.data(),v);
  }

void ObjectsBucket::free(const size_t objId) {
  freeList.push_back(objId);
  auto& v = val[objId];
  if(v.storageAni!=size_t(-1))
    storage.ani.free(v.storageAni);
  v.vboType = VboType::NoVbo;
  v.vbo     = nullptr;
  for(size_t i=0;i<Resources::MaxFramesInFlight;++i)
    v.vboM[i] = nullptr;
  v.vboA    = nullptr;
  v.ibo     = nullptr;
  }

void ObjectsBucket::draw(Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId) {
  if(pMain==nullptr || index.size()==0)
    return;

  if(useSharedUbo)
    p.setUniforms(*pMain,uboShared.ubo[fId]);

  UboPush pushBlock;
  for(auto pv:index) {
    auto& v = *pv;

    pushBlock.pos = v.pos;
    const size_t cnt = v.lightCnt;
    for(size_t r=0; r<cnt && r<LIGHT_BLOCK; ++r) {
      pushBlock.light[r].pos   = v.light[r]->position();
      pushBlock.light[r].color = v.light[r]->color();
      pushBlock.light[r].range = v.light[r]->range();
      }
    for(size_t r=cnt;r<LIGHT_BLOCK;++r) {
      pushBlock.light[r].range = 0;
      }

    p.setUniforms(*pMain,&pushBlock,sizeof(pushBlock));
    if(!useSharedUbo) {
      auto& ubo = v.ubo.ubo[fId];
      setAnim(v,ubo);
      if(v.storageAni!=size_t(-1))
        setUbo(v.ubo.uboBit[fId],ubo,3,storage.ani[fId],v.storageAni,1);
      p.setUniforms(*pMain,ubo);
      }

    switch(v.vboType) {
      case VboType::NoVbo:
        break;
      case VboType::VboVertex:
        p.draw(*v.vbo, *v.ibo);
        break;
      case VboType::VboVertexA:
        p.draw(*v.vboA,*v.ibo);
        break;
      case VboType::VboMorph:
        p.draw(*v.vboM[fId]);
        break;
      }
    }
  }

void ObjectsBucket::drawLight(Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId) {
  if(pLight==nullptr || index.size()==0)
    return;

  if(useSharedUbo)
    p.setUniforms(*pLight,uboShared.ubo[fId]);

  UboPush pushBlock;
  for(auto pv:index) {
    auto& v = *pv;
    if(v.lightCnt<=LIGHT_BLOCK)
      continue;

    if(!useSharedUbo) {
      p.setUniforms(*pLight,v.ubo.ubo[fId]);
      }
    pushBlock.pos = v.pos;

    for(size_t i=LIGHT_BLOCK; i<v.lightCnt; i+=LIGHT_BLOCK) {
      const size_t cnt = v.lightCnt-i;
      for(size_t r=0; r<cnt && r<LIGHT_BLOCK; ++r) {
        pushBlock.light[r].pos   = v.light[i+r]->position();
        pushBlock.light[r].color = v.light[i+r]->color();
        pushBlock.light[r].range = v.light[i+r]->range();
        }
      for(size_t r=cnt;r<LIGHT_BLOCK;++r) {
        pushBlock.light[r].range = 0;
        }
      p.setUniforms(*pLight,&pushBlock,sizeof(pushBlock));
      switch(v.vboType) {
        case VboType::NoVbo:
          break;
        case VboType::VboVertex:
          p.draw(*v.vbo, *v.ibo);
          break;
        case VboType::VboVertexA:
          p.draw(*v.vboA,*v.ibo);
          break;
        case VboType::VboMorph:
          p.draw(*v.vboM[fId]);
          break;
        }
      }
    }
  }

void ObjectsBucket::drawShadow(Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId, int layer) {
  if(pShadow==nullptr || index.size()==0)
    return;

  UboPush pushBlock = {};
  if(useSharedUbo)
    p.setUniforms(*pShadow,uboShared.uboSh[fId][layer]);

  for(auto pv:index) {
    auto& v = *pv;

    if(!useSharedUbo) {
      auto& ubo = v.ubo.uboSh[fId][layer];
      if(textureInShadowPass)
        setAnim(v,ubo);
      if(v.storageAni!=size_t(-1))
        setUbo(v.ubo.uboBitSh[fId][layer],ubo,3,storage.ani[fId],v.storageAni,1);
      p.setUniforms(*pShadow,ubo);
      }

    pushBlock.pos = v.pos;
    p.setUniforms(*pShadow,&pushBlock,sizeof(pushBlock));
    switch(v.vboType) {
      case VboType::NoVbo:
        break;
      case VboType::VboVertex:
        p.draw(*v.vbo, *v.ibo);
        break;
      case VboType::VboVertexA:
        p.draw(*v.vboA,*v.ibo);
        break;
      case VboType::VboMorph:
        p.draw(*v.vboM[fId]);
        break;
      }
    }
  }

void ObjectsBucket::draw(size_t id, Tempest::Encoder<Tempest::CommandBuffer>& p, uint8_t fId) {
  auto& v = val[id];
  if(v.vbo==nullptr || pMain==nullptr)
    return;

  storage.ani.commitUbo(scene.storage.device,fId);

  UboPush pushBlock = {};
  pushBlock.pos = v.pos;

  auto& ubo = useSharedUbo ? uboShared.ubo[fId] : v.ubo.ubo[fId];
  if(!useSharedUbo) {
    ubo.set(0,*mat.tex);
    ubo.set(1,Resources::fallbackTexture(),Sampler2d::nearest());
    ubo.set(2,scene.uboGlobalPf[fId][0],0,1);
    ubo.set(4,uboMat[fId]);
    }

  p.setUniforms(*pMain,ubo);
  p.setUniforms(*pMain,&pushBlock,sizeof(pushBlock));
  p.draw(*v.vbo, *v.ibo);
  }

void ObjectsBucket::setObjMatrix(size_t i, const Matrix4x4& m) {
  auto& v = val[i];
  v.bounds.setObjMatrix(m);
  v.pos = m;

  if(shaderType==Static)
    allBounds.r = 0;

  setupLights(v,false);
  }

void ObjectsBucket::setPose(size_t i, const Pose& p) {
  if(shaderType!=Animated)
    return;
  auto& v    = val[i];
  auto& skel = storage.ani.element(v.storageAni);

  std::memcpy(&skel.skel[0],p.tr.data(),p.tr.size()*sizeof(p.tr[0]));
  storage.ani.markAsChanged(v.storageAni);
  }

void ObjectsBucket::setBounds(size_t i, const Bounds& b) {
  val[i].bounds = b;
  }

void ObjectsBucket::setupLights(Object& val, bool noCache) {
  int cx = int(val.bounds.midTr.x/20.f);
  int cy = int(val.bounds.midTr.y/20.f);
  int cz = int(val.bounds.midTr.z/20.f);

  if(cx==val.lightCacheKey[0] &&
     cy==val.lightCacheKey[1] &&
     cz==val.lightCacheKey[2] &&
     !noCache)
    return;

  val.lightCacheKey[0] = cx;
  val.lightCacheKey[1] = cy;
  val.lightCacheKey[2] = cz;

  val.lightCnt = scene.lights.get(val.bounds,val.light,MAX_LIGHT);
  }

void ObjectsBucket::setAnim(ObjectsBucket::Object& v, Tempest::Uniforms& ubo) {
  if(mat.frames.size()==0)
    return;
  auto frame = size_t((v.timeShift+scene.tickCount)/mat.texAniFPSInv);
  auto t = mat.frames[frame%mat.frames.size()];
  ubo.set(0,*t);
  }

template<class T>
void ObjectsBucket::setUbo(uint8_t& bit, Tempest::Uniforms& ubo, uint8_t layoutBind,
                           const Tempest::UniformBuffer<T>& vbuf, size_t offset, size_t size) {
  const uint8_t flg = uint8_t(uint8_t(1)<<layoutBind);
  if(bit & flg)
    return;
  bit = uint8_t(bit | flg);
  ubo.set(layoutBind,vbuf,offset,size);
  }

void ObjectsBucket::setUbo(uint8_t& bit, Uniforms& ubo, uint8_t layoutBind,
                           const Texture2d& tex, const Sampler2d& smp) {
  const uint8_t flg = uint8_t(uint8_t(1)<<layoutBind);
  if(bit & flg)
    return;
  bit = uint8_t(bit | flg);
  ubo.set(layoutBind,tex,smp);
  }

bool ObjectsBucket::Storage::commitUbo(Device& device, uint8_t fId) {
  return ani.commitUbo(device,fId) | mat.commitUbo(device,fId);
  }

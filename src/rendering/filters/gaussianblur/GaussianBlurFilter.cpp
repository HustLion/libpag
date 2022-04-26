/////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Tencent is pleased to support the open source community by making libpag available.
//
//  Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  unless required by applicable law or agreed to in writing, software distributed under the
//  license is distributed on an "as is" basis, without warranties or conditions of any kind,
//  either express or implied. see the license for the specific language governing permissions
//  and limitations under the license.
//
/////////////////////////////////////////////////////////////////////////////////////////////////

#include "GaussianBlurFilter.h"
#include "rendering/filters/utils/FilterHelper.h"

namespace pag {
GaussianBlurFilter::GaussianBlurFilter(Effect* effect) : effect(effect) {
  auto* blurEffect = static_cast<FastBlurEffect*>(effect);
  
  blurParam.repeatEdgePixels = blurEffect->repeatEdgePixels->getValueAt(0);
  auto blurDimensions = blurEffect->blurDimensions->getValueAt(0);
  
  BlurOptions options = BlurOptions::None;
  
  if (blurParam.repeatEdgePixels) {
    options |= BlurOptions::RepeatEdgePixels;
  }
  
  if (blurDimensions == BlurDimensionsDirection::Vertical) {
    options |= BlurOptions::Vertical;
  } else if (blurDimensions == BlurDimensionsDirection::Horizontal) {
    options |= BlurOptions::Horizontal;
  } else {
    options |= BlurOptions::Horizontal | BlurOptions::Vertical;
  }

  downBlurPass = new GaussianBlurFilterPass(options | BlurOptions::Down);
  upBlurPass = new GaussianBlurFilterPass(options | BlurOptions::Up);
}

GaussianBlurFilter::~GaussianBlurFilter() {
  delete downBlurPass;
  delete upBlurPass;
}

bool GaussianBlurFilter::initialize(tgfx::Context* context) {
  if (!downBlurPass->initialize(context)) {
    return false;
  }
  if (!upBlurPass->initialize(context)) {
    return false;
  }
  
  return true;
}

void GaussianBlurFilter::updateBlurParam(float blurriness) {
  blurriness = blurriness < BLUR_LEVEL_MAX_LIMIT ?
               blurriness : BLUR_LEVEL_MAX_LIMIT;
  if (blurriness < BLUR_LEVEL_1_LIMIT) {
    blurParam.depth = BLUR_LEVEL_1_DEPTH;
    blurParam.scale = BLUR_LEVEL_1_SCALE;
    blurParam.value = blurriness / BLUR_LEVEL_1_LIMIT * 2.0;
  } else if (blurriness < BLUR_LEVEL_2_LIMIT) {
    blurParam.depth = BLUR_LEVEL_2_DEPTH;
    blurParam.scale = BLUR_LEVEL_2_SCALE;
    blurParam.value = (blurriness - BLUR_STABLE) / (BLUR_LEVEL_2_LIMIT - BLUR_STABLE) * 3.0;
  } else if (blurriness < BLUR_LEVEL_3_LIMIT) {
    blurParam.depth = BLUR_LEVEL_3_DEPTH;
    blurParam.scale = BLUR_LEVEL_3_SCALE;
    blurParam.value = (blurriness - BLUR_STABLE) / (BLUR_LEVEL_3_LIMIT - BLUR_STABLE) * 5.0;
  } else if (blurriness < BLUR_LEVEL_4_LIMIT) {
    blurParam.depth = BLUR_LEVEL_4_DEPTH;
    blurParam.scale = BLUR_LEVEL_4_SCALE;
    blurParam.value = (blurriness - BLUR_STABLE) / (BLUR_LEVEL_4_LIMIT - BLUR_STABLE) * 6.0;
  } else {
    blurParam.depth = BLUR_LEVEL_5_DEPTH;
    blurParam.scale = BLUR_LEVEL_5_SCALE;
    blurParam.value = 6.0 + (blurriness - BLUR_STABLE * 12.0) / (BLUR_LEVEL_5_LIMIT - BLUR_STABLE * 12.0) * 5.0;
  }
}

void GaussianBlurFilter::update(Frame frame, const tgfx::Rect& contentBounds,
                                const tgfx::Rect& transformedBounds, const tgfx::Point& filterScale) {
  LayerFilter::update(frame, contentBounds, transformedBounds, filterScale);

  auto blurriness = static_cast<FastBlurEffect*>(effect)->blurriness->getValueAt(layerFrame);
  updateBlurParam(blurriness);
  
  filtersBounds[0] = contentBounds;
  auto bounds = transformedBounds;
  for (int i = 0; i < blurParam.depth; i ++) {
    bounds = tgfx::Rect::MakeLTRB(bounds.left * blurParam.scale, bounds.top * blurParam.scale,
                                  bounds.right * blurParam.scale, bounds.bottom * blurParam.scale);
    filtersBounds[i + 1] = bounds;
    filtersBounds[blurParam.depth * 2 - 1 - i] = bounds;
  }
  filtersBounds[blurParam.depth * 2] = transformedBounds;
  filtersBoundsScale = filterScale;
}

void GaussianBlurFilter::draw(tgfx::Context* context, const FilterSource* source,
                           const FilterTarget* target) {
  if (source == nullptr || target == nullptr) {
    LOGE("GaussianBlurFilter::draw() can not draw filter");
    return;
  }
  
  std::unique_ptr<FilterSource> filterSourcePtr;
  std::unique_ptr<FilterTarget> filterTargetPtr;
  
  FilterSource* filterSource = const_cast<FilterSource*>(source);
  FilterTarget* filterTarget = nullptr;
  
  int boundsAnchor = 0;
  
  for (int i = 0; i < blurParam.depth; i ++) {
    auto sourceBounds = filtersBounds[boundsAnchor++];
    auto targetBounds = filtersBounds[boundsAnchor];
    auto filterBuffer = blurFilterBuffer[i];
    if (filterBuffer == nullptr ||
        filterBuffer->width() != targetBounds.width() ||
        filterBuffer->height() != targetBounds.height()) {
      filterBuffer = FilterBuffer::Make(context, targetBounds.width(), targetBounds.height());
      blurFilterBuffer[i] = filterBuffer;
    }
    if (filterBuffer == nullptr) {
      return;
    }
    filterBuffer->clearColor();
    auto offsetMatrix =
        tgfx::Matrix::MakeTrans((sourceBounds.left - targetBounds.left) * source->scale.x,
                                (sourceBounds.top - targetBounds.top) * source->scale.y);
    filterTargetPtr = filterBuffer->toFilterTarget(offsetMatrix);
    filterTarget = filterTargetPtr.get();
    downBlurPass->update(layerFrame, sourceBounds, targetBounds, filtersBoundsScale);
    downBlurPass->updateParams(blurParam.value);
    downBlurPass->draw(context, filterSource, filterTarget);
    filterSourcePtr = filterBuffer->toFilterSource(source->scale);
    filterSource = filterSourcePtr.get();
  }
  
  for (int i = blurParam.depth - 1; i >= 0; i --) {
    auto sourceBounds = filtersBounds[boundsAnchor++];
    auto targetBounds = filtersBounds[boundsAnchor];
    auto filterBuffer = i > 0 ? blurFilterBuffer[i - 1] : nullptr;
    auto offsetMatrix =
        tgfx::Matrix::MakeTrans((sourceBounds.left - targetBounds.left) * source->scale.x,
                                (sourceBounds.top - targetBounds.top) * source->scale.y);
    if (i == 0) {
      filterTarget = const_cast<FilterTarget*>(target);
      PreConcatMatrix(filterTarget, offsetMatrix);
    } else {
      filterBuffer->clearColor();
      filterTargetPtr = filterBuffer->toFilterTarget(offsetMatrix);
      filterTarget = filterTargetPtr.get();
    }
    upBlurPass->update(layerFrame, sourceBounds, targetBounds, filtersBoundsScale);
    upBlurPass->updateParams(blurParam.value);
    upBlurPass->draw(context, filterSource, filterTarget);
    if (i != 0) {
      filterSourcePtr = filterBuffer->toFilterSource(source->scale);
      filterSource = filterSourcePtr.get();
    }
  }
}
}  // namespace pag

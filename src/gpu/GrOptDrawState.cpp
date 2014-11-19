/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrOptDrawState.h"

#include "GrDefaultGeoProcFactory.h"
#include "GrDrawState.h"
#include "GrDrawTargetCaps.h"
#include "GrGpu.h"
#include "GrProcOptInfo.h"

GrOptDrawState::GrOptDrawState(const GrDrawState& drawState,
                               GrDrawState::BlendOpt blendOpt,
                               GrBlendCoeff optSrcCoeff,
                               GrBlendCoeff optDstCoeff,
                               GrGpu* gpu,
                               const ScissorState& scissorState,
                               const GrDeviceCoordTexture* dstCopy,
                               GrGpu::DrawType drawType)
: fRenderTarget(drawState.fRenderTarget.get()) {
    fScissorState = scissorState;
    fViewMatrix = drawState.getViewMatrix();
    fBlendConstant = drawState.getBlendConstant();
    fVAPtr = drawState.getVertexAttribs();
    fVACount = drawState.getVertexAttribCount();
    fVAStride = drawState.getVertexStride();
    fStencilSettings = drawState.getStencil();
    fDrawFace = drawState.getDrawFace();
    fSrcBlend = optSrcCoeff;
    fDstBlend = optDstCoeff;
    GrProgramDesc::DescInfo descInfo;

    fFlags = 0;
    if (drawState.isHWAntialias()) {
        fFlags |= kHWAA_Flag;
    }
    if (drawState.isColorWriteDisabled()) {
        fFlags |= kDisableColorWrite_Flag;
    }
    if (drawState.isDither()) {
        fFlags |= kDither_Flag;
    }

    memcpy(descInfo.fFixedFunctionVertexAttribIndices,
           drawState.getFixedFunctionVertexAttribIndices(),
           sizeof(descInfo.fFixedFunctionVertexAttribIndices));

    uint8_t fixedFunctionVAToRemove = 0;

    const GrProcOptInfo& colorPOI = drawState.colorProcInfo();
    int firstColorStageIdx = colorPOI.firstEffectiveStageIndex();
    descInfo.fInputColorIsUsed = colorPOI.inputColorIsUsed();
    fColor = colorPOI.inputColorToEffectiveStage();
    if (colorPOI.removeVertexAttrib()) {
        fixedFunctionVAToRemove |= 0x1 << kColor_GrVertexAttribBinding;
    }

    // TODO: Once we can handle single or four channel input into coverage stages then we can use
    // drawState's coverageProcInfo (like color above) to set this initial information.
    int firstCoverageStageIdx = 0;
    descInfo.fInputCoverageIsUsed = true;
    fCoverage = drawState.getCoverage();

    this->adjustProgramForBlendOpt(drawState, blendOpt, &descInfo, &firstColorStageIdx,
                                   &firstCoverageStageIdx, &fixedFunctionVAToRemove);
    // Should not be setting any more FFVA to be removed at this point
    if (0 != fixedFunctionVAToRemove) {
        this->removeFixedFunctionVertexAttribs(fixedFunctionVAToRemove, &descInfo);
    }
    this->getStageStats(drawState, firstColorStageIdx, firstCoverageStageIdx, &descInfo);

    // Copy GeometryProcesssor from DS or ODS
    SkASSERT(GrGpu::IsPathRenderingDrawType(drawType) ||
             GrGpu::kStencilPath_DrawType ||
             drawState.hasGeometryProcessor());
    fGeometryProcessor.reset(drawState.getGeometryProcessor());

    // Copy Stages from DS to ODS
    bool explicitLocalCoords = descInfo.hasLocalCoordAttribute();

    for (int i = firstColorStageIdx; i < drawState.numColorStages(); ++i) {
        SkNEW_APPEND_TO_TARRAY(&fFragmentStages,
                               GrPendingFragmentStage,
                               (drawState.fColorStages[i], explicitLocalCoords));
    }
    fNumColorStages = fFragmentStages.count();
    for (int i = firstCoverageStageIdx; i < drawState.numCoverageStages(); ++i) {
        SkNEW_APPEND_TO_TARRAY(&fFragmentStages,
                               GrPendingFragmentStage,
                               (drawState.fCoverageStages[i], explicitLocalCoords));
    }

    this->setOutputStateInfo(drawState, blendOpt, *gpu->caps(), &descInfo);

    // now create a key
    gpu->buildProgramDesc(*this, descInfo, drawType, dstCopy, &fDesc);
};

GrOptDrawState* GrOptDrawState::Create(const GrDrawState& drawState,
                                       GrGpu* gpu,
                                       const ScissorState& scissorState,
                                       const GrDeviceCoordTexture* dstCopy,
                                       GrGpu::DrawType drawType) {
    GrBlendCoeff srcCoeff;
    GrBlendCoeff dstCoeff;
    GrDrawState::BlendOpt blendOpt = drawState.getBlendOpt(false, &srcCoeff, &dstCoeff);

    // If our blend coeffs are set to 0,1 we know we will not end up drawing unless we are
    // stenciling. When path rendering the stencil settings are not always set on the draw state
    // so we must check the draw type. In cases where we will skip drawing we simply return a
    // null GrOptDrawState.
    if (kZero_GrBlendCoeff == srcCoeff && kOne_GrBlendCoeff == dstCoeff &&
        !drawState.getStencil().doesWrite() && GrGpu::kStencilPath_DrawType != drawType) {
        return NULL;
    }

    return SkNEW_ARGS(GrOptDrawState, (drawState, blendOpt, srcCoeff,
                                       dstCoeff, gpu, scissorState, dstCopy, drawType));
}

void GrOptDrawState::setOutputStateInfo(const GrDrawState& ds,
                                        GrDrawState::BlendOpt blendOpt,
                                        const GrDrawTargetCaps& caps,
                                        GrProgramDesc::DescInfo* descInfo) {
    // Set this default and then possibly change our mind if there is coverage.
    descInfo->fPrimaryOutputType = GrProgramDesc::kModulate_PrimaryOutputType;
    descInfo->fSecondaryOutputType = GrProgramDesc::kNone_SecondaryOutputType;

    // Determine whether we should use dual source blending or shader code to keep coverage
    // separate from color.
    bool keepCoverageSeparate = !(GrDrawState::kCoverageAsAlpha_BlendOpt == blendOpt ||
                                  GrDrawState::kEmitCoverage_BlendOpt == blendOpt);
    if (keepCoverageSeparate && !ds.hasSolidCoverage()) {
        if (caps.dualSourceBlendingSupport()) {
            if (kZero_GrBlendCoeff == fDstBlend) {
                // write the coverage value to second color
                descInfo->fSecondaryOutputType = GrProgramDesc::kCoverage_SecondaryOutputType;
                fDstBlend = (GrBlendCoeff)GrGpu::kIS2C_GrBlendCoeff;
            } else if (kSA_GrBlendCoeff == fDstBlend) {
                // SA dst coeff becomes 1-(1-SA)*coverage when dst is partially covered.
                descInfo->fSecondaryOutputType = GrProgramDesc::kCoverageISA_SecondaryOutputType;
                fDstBlend = (GrBlendCoeff)GrGpu::kIS2C_GrBlendCoeff;
            } else if (kSC_GrBlendCoeff == fDstBlend) {
                // SA dst coeff becomes 1-(1-SA)*coverage when dst is partially covered.
                descInfo->fSecondaryOutputType = GrProgramDesc::kCoverageISC_SecondaryOutputType;
                fDstBlend = (GrBlendCoeff)GrGpu::kIS2C_GrBlendCoeff;
            }
        } else if (descInfo->fReadsDst &&
                   kOne_GrBlendCoeff == fSrcBlend &&
                   kZero_GrBlendCoeff == fDstBlend) {
            descInfo->fPrimaryOutputType = GrProgramDesc::kCombineWithDst_PrimaryOutputType;
        }
    }
}

void GrOptDrawState::adjustProgramForBlendOpt(const GrDrawState& ds,
                                              GrDrawState::BlendOpt blendOpt,
                                              GrProgramDesc::DescInfo* descInfo,
                                              int* firstColorStageIdx,
                                              int* firstCoverageStageIdx,
                                              uint8_t* fixedFunctionVAToRemove) {
    switch (blendOpt) {
        case GrDrawState::kNone_BlendOpt:
        case GrDrawState::kSkipDraw_BlendOpt:
        case GrDrawState::kCoverageAsAlpha_BlendOpt:
            break;
        case GrDrawState::kEmitCoverage_BlendOpt:
            fColor = 0xffffffff;
            descInfo->fInputColorIsUsed = true;
            *firstColorStageIdx = ds.numColorStages();
            *fixedFunctionVAToRemove |= 0x1 << kColor_GrVertexAttribBinding;
            break;
        case GrDrawState::kEmitTransBlack_BlendOpt:
            fColor = 0;
            fCoverage = 0xff;
            descInfo->fInputColorIsUsed = true;
            descInfo->fInputCoverageIsUsed = true;
            *firstColorStageIdx = ds.numColorStages();
            *firstCoverageStageIdx = ds.numCoverageStages();
            *fixedFunctionVAToRemove |= (0x1 << kColor_GrVertexAttribBinding |
                                         0x1 << kCoverage_GrVertexAttribBinding);
            break;
    }
}

void GrOptDrawState::removeFixedFunctionVertexAttribs(uint8_t removeVAFlag,
                                                      GrProgramDesc::DescInfo* descInfo) {
    int numToRemove = 0;
    uint8_t maskCheck = 0x1;
    // Count the number of vertex attributes that we will actually remove
    for (int i = 0; i < kGrFixedFunctionVertexAttribBindingCnt; ++i) {
        if ((maskCheck & removeVAFlag) && -1 != descInfo->fFixedFunctionVertexAttribIndices[i]) {
            ++numToRemove;
        }
        maskCheck <<= 1;
    }

    fOptVA.reset(fVACount - numToRemove);

    GrVertexAttrib* dst = fOptVA.get();
    const GrVertexAttrib* src = fVAPtr;

    for (int i = 0, newIdx = 0; i < fVACount; ++i, ++src) {
        const GrVertexAttrib& currAttrib = *src;
        if (currAttrib.fBinding < kGrFixedFunctionVertexAttribBindingCnt) {
            uint8_t maskCheck = 0x1 << currAttrib.fBinding;
            if (maskCheck & removeVAFlag) {
                SkASSERT(-1 != descInfo->fFixedFunctionVertexAttribIndices[currAttrib.fBinding]);
                descInfo->fFixedFunctionVertexAttribIndices[currAttrib.fBinding] = -1;
                continue;
            }
            descInfo->fFixedFunctionVertexAttribIndices[currAttrib.fBinding] = newIdx;
        }
        memcpy(dst, src, sizeof(GrVertexAttrib));
        ++newIdx;
        ++dst;
    }
    fVACount -= numToRemove;
    fVAPtr = fOptVA.get();
}

static void get_stage_stats(const GrFragmentStage& stage, bool* readsDst, bool* readsFragPosition) {
    if (stage.getProcessor()->willReadDstColor()) {
        *readsDst = true;
    }
    if (stage.getProcessor()->willReadFragmentPosition()) {
        *readsFragPosition = true;
    }
}

void GrOptDrawState::getStageStats(const GrDrawState& ds, int firstColorStageIdx,
                                   int firstCoverageStageIdx, GrProgramDesc::DescInfo* descInfo) {
    // We will need a local coord attrib if there is one currently set on the optState and we are
    // actually generating some effect code
    descInfo->fRequiresLocalCoordAttrib = descInfo->hasLocalCoordAttribute() &&
        ds.numTotalStages() - firstColorStageIdx - firstCoverageStageIdx > 0;

    descInfo->fReadsDst = false;
    descInfo->fReadsFragPosition = false;

    for (int s = firstColorStageIdx; s < ds.numColorStages(); ++s) {
        const GrFragmentStage& stage = ds.getColorStage(s);
        get_stage_stats(stage, &descInfo->fReadsDst, &descInfo->fReadsFragPosition);
    }
    for (int s = firstCoverageStageIdx; s < ds.numCoverageStages(); ++s) {
        const GrFragmentStage& stage = ds.getCoverageStage(s);
        get_stage_stats(stage, &descInfo->fReadsDst, &descInfo->fReadsFragPosition);
    }
    if (ds.hasGeometryProcessor()) {
        const GrGeometryProcessor& gp = *ds.getGeometryProcessor();
        descInfo->fReadsFragPosition = descInfo->fReadsFragPosition || gp.willReadFragmentPosition();
    }
}

////////////////////////////////////////////////////////////////////////////////

bool GrOptDrawState::operator== (const GrOptDrawState& that) const {
    if (this->fDesc != that.fDesc) {
        return false;
    }
    bool usingVertexColors = that.fDesc.header().fColorAttributeIndex != -1;
    if (!usingVertexColors && this->fColor != that.fColor) {
        return false;
    }

    if (this->getRenderTarget() != that.getRenderTarget() ||
        this->fScissorState != that.fScissorState ||
        !this->fViewMatrix.cheapEqualTo(that.fViewMatrix) ||
        this->fSrcBlend != that.fSrcBlend ||
        this->fDstBlend != that.fDstBlend ||
        this->fBlendConstant != that.fBlendConstant ||
        this->fFlags != that.fFlags ||
        this->fVACount != that.fVACount ||
        this->fVAStride != that.fVAStride ||
        memcmp(this->fVAPtr, that.fVAPtr, this->fVACount * sizeof(GrVertexAttrib)) ||
        this->fStencilSettings != that.fStencilSettings ||
        this->fDrawFace != that.fDrawFace) {
        return false;
    }

    bool usingVertexCoverage = this->fDesc.header().fCoverageAttributeIndex != -1;
    if (!usingVertexCoverage && this->fCoverage != that.fCoverage) {
        return false;
    }

    if (this->hasGeometryProcessor()) {
        if (!that.hasGeometryProcessor()) {
            return false;
        } else if (!this->getGeometryProcessor()->isEqual(*that.getGeometryProcessor())) {
            return false;
        }
    } else if (that.hasGeometryProcessor()) {
        return false;
    }

    // The program desc comparison should have already assured that the stage counts match.
    SkASSERT(this->numFragmentStages() == that.numFragmentStages());
    for (int i = 0; i < this->numFragmentStages(); i++) {

        if (this->getFragmentStage(i) != that.getFragmentStage(i)) {
            return false;
        }
    }
    return true;
}


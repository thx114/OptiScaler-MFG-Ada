#pragma once
// 图像行翻转，不修改运动矢量数值或单位。
namespace NativeGuideRows {
    template<class CopyRow>
    void Flip(unsigned height, CopyRow&& copy) {
        for(unsigned source=0;source<height;source++) copy(source,height-1-source);
    }
}

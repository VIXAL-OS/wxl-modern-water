// Analytic surface-normal rings: no rain particles, simulation grid, texture reads,
// or extra render pass. Each cell's ring stays inside its boundary, so two
// rotated layers need only two candidates per shaded water pixel.
float3 RainHash(float2 cell)
{
    float3 p=frac(float3(cell.x,cell.y,cell.x)*float3(0.1031,0.1030,0.0973));
    p+=dot(p,p.yxz+33.33);
    return frac((p.xxy+p.yzz)*p.zyx);
}

// xy = normal slope, z = a short-lived crest for diffuse splash lighting.
float3 RainRing(float2 position, float seconds, float2 offset, float size, float footprint)
{
    float spacing=1.5*size;
    float2 q=position/spacing+offset;
    float2 cell=floor(q);
    float3 seed=RainHash(cell);
    // Integer cycles per 256 seconds preserve continuity when the CPU clock wraps.
    // Every cell has a different cadence; each new drop gets a fresh position.
    float cycles=128+floor(seed.z*128);
    float phase=seconds*(cycles/256)+seed.x;
    float cycle=floor(phase);
    cycle-=cycles*floor(cycle/cycles);
    float3 h=RainHash(cell+cycle*float2(17.17,31.71));
    float ringSize=size*(0.65+0.55*h.z);
    float width=clamp(footprint*0.5,0.075*size,0.10*size);
    // Smaller rings can land farther from the cell centre. Reserve enough margin
    // for the whole ring so neighbouring cells never clip its expanding edge.
    float margin=(0.46*ringSize+width)/spacing+0.001;
    float2 centre=lerp(margin,1-margin,h.xy);
    float2 delta=(frac(q)-centre)*spacing;
    float distance=length(delta);
    // A varying quiet interval avoids a constantly occupied lattice of rings.
    // Both endpoints have zero amplitude before the next position is selected.
    float age=saturate(frac(phase)/(0.72+0.23*seed.y));
    float radius=(0.035+0.425*age)*ringSize;
    float x=clamp((distance-radius)/width,-1,1);
    float envelope=saturate(age*12)*(1-age)*(1-age)*(0.7+0.3*h.y);
    // Derivative of a compact quartic ring: continuous at both band edges.
    float band=1-x*x;
    float slope=-4*x*band/width*(0.006*ringSize)*envelope;
    return float3(delta/max(distance,0.0001)*slope,band*band*envelope);
}

float3 SampleRain(WaterPixel w)
{
    float size=max(cRainControl.z,0.5);
    float range=max(cRainControl.w,24);
    [branch] if (cRainControl.x<=0 || w.cameraDistance>=range) return 0;
    float footprint=max(length(w.footprintX),length(w.footprintY));
    float fade=saturate((range-w.cameraDistance)/(range*0.5)) *
               saturate((0.50*size-footprint)/(0.35*size));
    [branch] if (fade<=0) return 0;
    float3 a=RainRing(w.position.xy,cRainControl.y,float2(0,0),size,footprint);
    // A differently oriented, differently spaced layer breaks up visible rows.
    // Rotate its normal slope back into world space before combining the layers.
    const float cs=0.81915204, sn=0.57357644;
    float2 rotated=float2(cs*w.position.x-sn*w.position.y,
                          sn*w.position.x+cs*w.position.y);
    float3 b=RainRing(rotated,cRainControl.y,float2(0.5,0.37),size*1.071,footprint);
    b.xy=float2(cs*b.x+sn*b.y,-sn*b.x+cs*b.y);
    return (a+b)*(cRainControl.x*fade);
}

float2 RainSlope(WaterPixel w) { return SampleRain(w).xy; }

param([string]$BaselineLabel='stage75-linear',[string]$CandidateLabel='stage75-accelerated',[int]$Width=640,[int]$Height=400)
$ErrorActionPreference='Stop'
$taskProjectRoot=(Resolve-Path -LiteralPath (Split-Path $PSScriptRoot -Parent)).Path
Push-Location -LiteralPath $taskProjectRoot
try {
    if(-not ('VXGIStage75FloatDifference' -as [type])){
        Add-Type -TypeDefinition @'
using System;
using System.IO;
public sealed class VXGIStage75FloatDifference {
    public long pixels,changedRgbPixels,changedAlphaPixels,nonfiniteChannels;
    public double maximumRgbAbsoluteError,meanRgbAbsoluteError,maximumAlphaAbsoluteError,rgbRootMeanSquareError;
    public double baselineMeanLuminance,candidateMeanLuminance,meanAbsoluteLuminanceError;
    public long foregroundPixels;
    public double foregroundMeanRgbAbsoluteError,foregroundRgbRootMeanSquareError,foregroundBaselineMeanLuminance,foregroundCandidateMeanLuminance,foregroundMeanAbsoluteLuminanceError;
    public static VXGIStage75FloatDifference Compare(string aPath,string bPath,int width,int height,string depthPath) {
        var a=File.ReadAllBytes(aPath);var b=File.ReadAllBytes(bPath);
        var depth=String.IsNullOrEmpty(depthPath)?null:File.ReadAllBytes(depthPath);
        long expected=(long)width*height*16;
        if(a.Length!=expected||b.Length!=expected)throw new InvalidDataException("HDR float4 file size differs from expected resolution");
        if(depth!=null&&depth.Length!=(long)width*height*4)throw new InvalidDataException("D32 mask differs from expected resolution");
        var result=new VXGIStage75FloatDifference();result.pixels=(long)width*height;
        double sum=0,squared=0;
        for(int i=0;i<a.Length;i+=16) {
            bool changedRgb=false;
            double aLuminance=0,bLuminance=0;
            bool foreground=depth!=null&&BitConverter.ToSingle(depth,i/4)<1;
            for(int c=0;c<4;++c) {
                float x=BitConverter.ToSingle(a,i+c*4),y=BitConverter.ToSingle(b,i+c*4);
                if(float.IsNaN(x)||float.IsNaN(y)||float.IsInfinity(x)||float.IsInfinity(y)){++result.nonfiniteChannels;continue;}
                double error=Math.Abs((double)x-y);
                if(c<3){double weight=c==0?0.2126:c==1?0.7152:0.0722;aLuminance+=weight*x;bLuminance+=weight*y;sum+=error;squared+=error*error;result.maximumRgbAbsoluteError=Math.Max(result.maximumRgbAbsoluteError,error);changedRgb|=x!=y;if(foreground){result.foregroundMeanRgbAbsoluteError+=error;result.foregroundRgbRootMeanSquareError+=error*error;}}
                else {result.maximumAlphaAbsoluteError=Math.Max(result.maximumAlphaAbsoluteError,error);if(x!=y)++result.changedAlphaPixels;}
            }
            if(changedRgb)++result.changedRgbPixels;
            result.baselineMeanLuminance+=aLuminance;result.candidateMeanLuminance+=bLuminance;result.meanAbsoluteLuminanceError+=Math.Abs(aLuminance-bLuminance);
            if(foreground){++result.foregroundPixels;result.foregroundBaselineMeanLuminance+=aLuminance;result.foregroundCandidateMeanLuminance+=bLuminance;result.foregroundMeanAbsoluteLuminanceError+=Math.Abs(aLuminance-bLuminance);}
        }
        result.meanRgbAbsoluteError=sum/(3*result.pixels);result.rgbRootMeanSquareError=Math.Sqrt(squared/(3*result.pixels));result.baselineMeanLuminance/=result.pixels;result.candidateMeanLuminance/=result.pixels;result.meanAbsoluteLuminanceError/=result.pixels;
        if(result.foregroundPixels>0){result.foregroundMeanRgbAbsoluteError/=3*result.foregroundPixels;result.foregroundRgbRootMeanSquareError=Math.Sqrt(result.foregroundRgbRootMeanSquareError/(3*result.foregroundPixels));result.foregroundBaselineMeanLuminance/=result.foregroundPixels;result.foregroundCandidateMeanLuminance/=result.foregroundPixels;result.foregroundMeanAbsoluteLuminanceError/=result.foregroundPixels;}return result;
    }
    public static byte[] DifferencePreview(string aPath,string bPath,int width,int height,int stride) {
        var a=File.ReadAllBytes(aPath);var b=File.ReadAllBytes(bPath);var pixels=new byte[stride*height];
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)for(int c=0;c<3;++c) {
            int offset=(y*width+x)*16+c*4;double v=Math.Abs((double)BitConverter.ToSingle(a,offset)-BitConverter.ToSingle(b,offset))*8;
            if(double.IsNaN(v)||double.IsInfinity(v))v=1;v=v/(1+v);v=v<=0.0031308?12.92*v:1.055*Math.Pow(v,1/2.4)-0.055;
            pixels[y*stride+x*3+(2-c)]=(byte)Math.Max(0,Math.Min(255,v*255+0.5));
        }
        return pixels;
    }
}
'@
    }
    $taskBaseline=Get-Content -LiteralPath ('tmp/VXGIStage75-'+$BaselineLabel+'-'+$Width+'x'+$Height+'-report.json') -Raw | ConvertFrom-Json
    $taskCandidate=Get-Content -LiteralPath ('tmp/VXGIStage75-'+$CandidateLabel+'-'+$Width+'x'+$Height+'-report.json') -Raw | ConvertFrom-Json
    if($taskBaseline.raw_snapshot_sha256 -ne $taskCandidate.raw_snapshot_sha256){throw 'A/B uses different RAW contents.'}
    $taskBaselinePipeline=$taskBaseline.pipeline | Select-Object * -ExcludeProperty trace_downsample_factor
    $taskCandidatePipeline=$taskCandidate.pipeline | Select-Object * -ExcludeProperty trace_downsample_factor
    if(($taskBaselinePipeline | ConvertTo-Json -Compress) -ne ($taskCandidatePipeline | ConvertTo-Json -Compress)){throw 'A/B uses different GI settings beyond requested trace factor.'}
    if($taskBaseline.captures.Count -ne $taskCandidate.captures.Count){throw 'A/B captures differ.'}
    $taskResults=@()
    Add-Type -AssemblyName System.Drawing
    foreach($taskCapture in $taskBaseline.captures){
        $taskOther=@($taskCandidate.captures | Where-Object {$_.camera -eq $taskCapture.camera -and $_.mode -eq $taskCapture.mode -and $_.sample -eq $taskCapture.sample})
        if($taskOther.Count -ne 1){throw 'Missing or duplicate same-camera/mode/frame comparison.'}
        $taskDepthPath=''
        if($taskCapture.raster_depth){
            $taskDepthHashA=Get-FileHash -LiteralPath $taskCapture.raster_depth -Algorithm SHA256
            $taskDepthHashB=Get-FileHash -LiteralPath $taskOther[0].raster_depth -Algorithm SHA256
            if($taskDepthHashA.Hash -ne $taskDepthHashB.Hash){throw 'A/B raster depth differs; foreground mask and receiver images do not match.'}
            $taskDepthPath=Join-Path $taskProjectRoot $taskCapture.raster_depth
        }
        $taskDifference=[VXGIStage75FloatDifference]::Compare((Join-Path $taskProjectRoot $taskCapture.linear_hdr),(Join-Path $taskProjectRoot $taskOther[0].linear_hdr),$Width,$Height,$taskDepthPath)
        $taskHashA=Get-FileHash -LiteralPath $taskCapture.linear_hdr -Algorithm SHA256
        $taskHashB=Get-FileHash -LiteralPath $taskOther[0].linear_hdr -Algorithm SHA256
        $taskDiffPath='tmp/vxgi-stage75-performance/'+$BaselineLabel+'-vs-'+$CandidateLabel+'-'+$Width+'x'+$Height+'-camera'+$taskCapture.camera+'_mode'+$taskCapture.mode+'_absolute_difference_x8.png'
        $taskBitmap=[Drawing.Bitmap]::new($Width,$Height,[Drawing.Imaging.PixelFormat]::Format24bppRgb)
        try {
            $taskLock=$taskBitmap.LockBits([Drawing.Rectangle]::new(0,0,$Width,$Height),[Drawing.Imaging.ImageLockMode]::WriteOnly,[Drawing.Imaging.PixelFormat]::Format24bppRgb)
            try {$taskBytes=[VXGIStage75FloatDifference]::DifferencePreview((Join-Path $taskProjectRoot $taskCapture.linear_hdr),(Join-Path $taskProjectRoot $taskOther[0].linear_hdr),$Width,$Height,$taskLock.Stride);[Runtime.InteropServices.Marshal]::Copy($taskBytes,0,$taskLock.Scan0,$taskBytes.Length)}
            finally {$taskBitmap.UnlockBits($taskLock)}
            $taskBitmap.Save((Join-Path $taskProjectRoot $taskDiffPath),[Drawing.Imaging.ImageFormat]::Png)
        }finally {$taskBitmap.Dispose()}
        $taskResults+=[pscustomobject]@{camera=$taskCapture.camera;mode=$taskCapture.mode;sample=$taskCapture.sample;pixels=$taskDifference.pixels;changed_rgb_pixels=$taskDifference.changedRgbPixels;changed_alpha_pixels=$taskDifference.changedAlphaPixels;maximum_rgb_absolute_error=$taskDifference.maximumRgbAbsoluteError;mean_rgb_absolute_error=$taskDifference.meanRgbAbsoluteError;rgb_root_mean_square_error=$taskDifference.rgbRootMeanSquareError;baseline_mean_luminance=$taskDifference.baselineMeanLuminance;candidate_mean_luminance=$taskDifference.candidateMeanLuminance;mean_absolute_luminance_error=$taskDifference.meanAbsoluteLuminanceError;maximum_alpha_absolute_error=$taskDifference.maximumAlphaAbsoluteError;nonfinite_channels=$taskDifference.nonfiniteChannels;baseline_hdr_sha256=$taskHashA.Hash;candidate_hdr_sha256=$taskHashB.Hash;byte_identical=$taskHashA.Hash -eq $taskHashB.Hash;absolute_difference_x8_preview=$taskDiffPath}
        $taskResults[-1] | Add-Member -NotePropertyName foreground_mask_available -NotePropertyValue ([bool]$taskDepthPath)
        if($taskDepthPath){$taskResults[-1] | Add-Member -NotePropertyMembers @{foreground_pixels=$taskDifference.foregroundPixels;foreground_mean_rgb_absolute_error=$taskDifference.foregroundMeanRgbAbsoluteError;foreground_rgb_root_mean_square_error=$taskDifference.foregroundRgbRootMeanSquareError;foreground_baseline_mean_luminance=$taskDifference.foregroundBaselineMeanLuminance;foreground_candidate_mean_luminance=$taskDifference.foregroundCandidateMeanLuminance;foreground_mean_absolute_luminance_error=$taskDifference.foregroundMeanAbsoluteLuminanceError}}
    }
    $taskReport=[pscustomobject]@{baseline_label=$BaselineLabel;candidate_label=$CandidateLabel;resolution=@($Width,$Height);baseline_downsample_factor=if($taskBaseline.downsample_factor){$taskBaseline.downsample_factor}else{2};candidate_downsample_factor=if($taskCandidate.downsample_factor){$taskCandidate.downsample_factor}else{2};baseline_debug_shader_policy=[bool]$taskBaseline.debug_shader_policy;candidate_debug_shader_policy=[bool]$taskCandidate.debug_shader_policy;raw_snapshot_sha256=$taskBaseline.raw_snapshot_sha256;captures=$taskResults;contract='Identical captured RAW, camera, mode, SSGI sample frame and GI settings except the explicitly reported trace factor. Shader compilation policies are reported separately and may differ. Linear HDR float4 comparison after production raster+GI; timing is separate. Absolute-difference preview uses gain 8, Reinhard then sRGB. This is not a ground-truth quality test.'}
    $taskReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath ('tmp/VXGIStage75-'+$BaselineLabel+'-vs-'+$CandidateLabel+'-'+$Width+'x'+$Height+'-comparison.json') -Encoding utf8
    $taskResults | Format-Table -AutoSize
}
finally {Pop-Location}

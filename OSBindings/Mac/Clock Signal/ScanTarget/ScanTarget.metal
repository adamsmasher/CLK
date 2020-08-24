//
//  ScanTarget.metal
//  Clock Signal
//
//  Created by Thomas Harte on 04/08/2020.
//  Copyright © 2020 Thomas Harte. All rights reserved.
//

#include <metal_stdlib>
using namespace metal;

// TODO: I'm being very loose, so far, in use of alpha. Sometimes it's 0.64, somtimes its 1.0.
// Apply some rigour, for crying out loud.

struct Uniforms {
	// This is used to scale scan positions, i.e. it provides the range
	// for mapping from scan-style integer positions into eye space.
	int2 scale;

	// This provides the intended height of a scan, in eye-coordinate terms.
	float lineWidth;

	// Provides a scaling factor in order to preserve 4:3 central content.
	float aspectRatioMultiplier;

	// Provides conversions to and from RGB for the active colour space.
	float3x3 toRGB;
	float3x3 fromRGB;

	// Provides zoom and offset to scale the source data.
	float zoom;
	float2 offset;

	// Describes the FIR filter in use; it'll be 15 coefficients but they're
	// symmetrical around the centre.
	float3 firCoefficients[8];
};

namespace {

constexpr sampler standardSampler(	coord::pixel,
									address::clamp_to_edge,	// Although arbitrary, stick with this address mode for compatibility all the way to MTLFeatureSet_iOS_GPUFamily1_v1.
									filter::nearest);

}

// MARK: - Structs used for receiving data from the emulation.

// This is intended to match the net effect of `Scan` as defined by the BufferingScanTarget.
struct Scan {
	struct EndPoint {
		uint16_t position[2];
		uint16_t dataOffset;
		uint16_t compositeAngle;
		uint16_t cyclesSinceRetrace;
	} endPoints[2];

	uint8_t compositeAmplitude;
	uint16_t dataY;
	uint16_t line;
};

// This matches the BufferingScanTarget's `Line`.
struct Line {
	struct EndPoint {
		uint16_t position[2];
		uint16_t compositeAngle;
		uint16_t cyclesSinceRetrace;
	} endPoints[2];

	uint8_t compositeAmplitude;
	uint16_t line;
};

// MARK: - Intermediate structs.

struct SourceInterpolator {
	float4 position [[position]];
	float2 textureCoordinates;
	float colourPhase;
	float colourAmplitude;
};


// MARK: - Vertex shaders.

float2 textureLocation(constant Line *line, float offset) {
	return float2(
		mix(line->endPoints[0].cyclesSinceRetrace, line->endPoints[1].cyclesSinceRetrace, offset),
		line->line);
}

float2 textureLocation(constant Scan *scan, float offset) {
	return float2(
		mix(scan->endPoints[0].dataOffset, scan->endPoints[1].dataOffset, offset),
		scan->dataY);
}

template <typename Input> SourceInterpolator toDisplay(
	constant Uniforms &uniforms [[buffer(1)]],
	constant Input *inputs [[buffer(0)]],
	uint instanceID [[instance_id]],
	uint vertexID [[vertex_id]]) {
	SourceInterpolator output;

	// Get start and end vertices in regular float2 form.
	const float2 start = float2(
		float(inputs[instanceID].endPoints[0].position[0]) / float(uniforms.scale.x),
		float(inputs[instanceID].endPoints[0].position[1]) / float(uniforms.scale.y)
	);
	const float2 end = float2(
		float(inputs[instanceID].endPoints[1].position[0]) / float(uniforms.scale.x),
		float(inputs[instanceID].endPoints[1].position[1]) / float(uniforms.scale.y)
	);

	// Calculate the tangent and normal.
	const float2 tangent = (end - start);
	const float2 normal = float2(tangent.y, -tangent.x) / length(tangent);

	// Load up the colour details.
	output.colourAmplitude = float(inputs[instanceID].compositeAmplitude) / 255.0f;
	output.colourPhase = 3.141592654f * mix(
		float(inputs[instanceID].endPoints[0].compositeAngle),
		float(inputs[instanceID].endPoints[1].compositeAngle),
		float((vertexID&2) >> 1)
	) / 32.0f;

	// Hence determine this quad's real shape, using vertexID to pick a corner.

	// position2d is now in the range [0, 1].
	float2 position2d = start + (float(vertexID&2) * 0.5f) * tangent + (float(vertexID&1) - 0.5f) * normal * uniforms.lineWidth;

	// Apply the requested offset and zoom, to map the desired area to the range [0, 1].
	position2d = (position2d + uniforms.offset) * uniforms.zoom;

	// Remap from [0, 1] to Metal's [-1, 1] and then apply the aspect ratio correction.
	position2d = (position2d * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f)) * float2(uniforms.aspectRatioMultiplier, 1.0f);

	output.position = float4(
		position2d,
		0.0f,
		1.0f
	);
	output.textureCoordinates = textureLocation(&inputs[instanceID], float((vertexID&2) >> 1));

	return output;
}

// These next two assume the incoming geometry to be a four-vertex triangle strip; each instance will therefore
// produce a quad.

vertex SourceInterpolator scanToDisplay(	constant Uniforms &uniforms [[buffer(1)]],
											constant Scan *scans [[buffer(0)]],
											uint instanceID [[instance_id]],
											uint vertexID [[vertex_id]]) {
	return toDisplay(uniforms, scans, instanceID, vertexID);
}

vertex SourceInterpolator lineToDisplay(	constant Uniforms &uniforms [[buffer(1)]],
											constant Line *lines [[buffer(0)]],
											uint instanceID [[instance_id]],
											uint vertexID [[vertex_id]]) {
	return toDisplay(uniforms, lines, instanceID, vertexID);
}

// This assumes that it needs to generate endpoints for a line segment.

vertex SourceInterpolator scanToComposition(	constant Uniforms &uniforms [[buffer(1)]],
												constant Scan *scans [[buffer(0)]],
												uint instanceID [[instance_id]],
												uint vertexID [[vertex_id]],
												texture2d<float> texture [[texture(0)]]) {
	SourceInterpolator result;

	// Populate result as if direct texture access were available.
	result.position.x = mix(scans[instanceID].endPoints[0].cyclesSinceRetrace, scans[instanceID].endPoints[1].cyclesSinceRetrace, float(vertexID));
	result.position.y = scans[instanceID].line;
	result.position.zw = float2(0.0f, 1.0f);
	result.textureCoordinates.x = mix(scans[instanceID].endPoints[0].dataOffset, scans[instanceID].endPoints[1].dataOffset, float(vertexID));
	result.textureCoordinates.y = scans[instanceID].dataY;
	result.colourPhase = 3.141592654f * mix(
		float(scans[instanceID].endPoints[0].compositeAngle),
		float(scans[instanceID].endPoints[1].compositeAngle),
		float(vertexID)
	) / 32.0f;
	result.colourAmplitude = float(scans[instanceID].compositeAmplitude) / 255.0f;

	// Map position into eye space, allowing for target texture dimensions.
	// TODO: is this really necessary? Is there nothing like coord::pixel that applies here?
	result.position.xy = ((result.position.xy + float2(0.5)) / float2(texture.get_width(), texture.get_height())) * float2(2.0, -2.0) + float2(-1.0, 1.0);

	return result;
}


// MARK: - Various input format conversion samplers.

// There's only one meaningful way to sample the luminance formats.

fragment float4 sampleLuminance1(SourceInterpolator vert [[stage_in]], texture2d<ushort> texture [[texture(0)]]) {
	return float4(float3(texture.sample(standardSampler, vert.textureCoordinates).r), 1.0);
}

fragment float4 sampleLuminance8(SourceInterpolator vert [[stage_in]], texture2d<float> texture [[texture(0)]]) {
	return float4(float3(texture.sample(standardSampler, vert.textureCoordinates).r), 1.0);
}

fragment float4 samplePhaseLinkedLuminance8(SourceInterpolator vert [[stage_in]], texture2d<float> texture [[texture(0)]]) {
	const int offset = int(vert.colourPhase * 4.0);
	auto sample = texture.sample(standardSampler, vert.textureCoordinates);
	return float4(float3(sample[offset]), 1.0);
}

// The luminance/phase format can produce either composite or S-Video.

fragment float4 sampleLuminance8Phase8(SourceInterpolator vert [[stage_in]], texture2d<float> texture [[texture(0)]]) {
	return float4(texture.sample(standardSampler, vert.textureCoordinates).rg, 0.0, 1.0);
}

fragment float4 compositeSampleLuminance8Phase8(SourceInterpolator vert [[stage_in]], texture2d<float> texture [[texture(0)]]) {
	const auto luminancePhase = texture.sample(standardSampler, vert.textureCoordinates).rg;
	const float phaseOffset = 3.141592654 * 4.0 * luminancePhase.g;
	const float rawChroma = step(luminancePhase.g, 0.75) * cos(vert.colourPhase + phaseOffset);
	const float level = mix(luminancePhase.r, rawChroma, vert.colourAmplitude);
	return float4(
		level,
		0.5 + 0.5*level*cos(vert.colourPhase),
		0.5 + 0.5*level*sin(vert.colourPhase),
		1.0
	);
}

// All the RGB formats can produce RGB, composite or S-Video.
//
// Note on the below: in Metal you may not call a fragment function (so e.g. svideoSampleX can't just cann sampleX).
// Also I can find no functioning way to offer a templated fragment function. So I don't currently know how
// I could avoid the macro mess below.

// TODO: is the calling convention here causing `vert` and `texture` to be copied?
float3 convertRed8Green8Blue8(SourceInterpolator vert, texture2d<float> texture) {
	return float3(texture.sample(standardSampler, vert.textureCoordinates));
}

float3 convertRed4Green4Blue4(SourceInterpolator vert, texture2d<ushort> texture) {
	const auto sample = texture.sample(standardSampler, vert.textureCoordinates).rg;
	return float3(sample.r&15, (sample.g >> 4)&15, sample.g&15);
}

float3 convertRed2Green2Blue2(SourceInterpolator vert, texture2d<ushort> texture) {
	const auto sample = texture.sample(standardSampler, vert.textureCoordinates).r;
	return float3((sample >> 4)&3, (sample >> 2)&3, sample&3);
}

float3 convertRed1Green1Blue1(SourceInterpolator vert, texture2d<ushort> texture) {
	const auto sample = texture.sample(standardSampler, vert.textureCoordinates).r;
	return float3(sample&4, sample&2, sample&1);
}

// TODO: don't hard code the 0.64 in sample##name.
#define DeclareShaders(name, pixelType)	\
	fragment float4 sample##name(SourceInterpolator vert [[stage_in]], texture2d<pixelType> texture [[texture(0)]]) {	\
		return float4(convert##name(vert, texture), 0.64);	\
	}	\
	\
	fragment float4 svideoSample##name(SourceInterpolator vert [[stage_in]], texture2d<pixelType> texture [[texture(0)]], constant Uniforms &uniforms [[buffer(0)]]) {	\
		const auto colour = uniforms.fromRGB * convert##name(vert, texture);	\
		const float2 colourSubcarrier = float2(cos(vert.colourPhase), sin(vert.colourPhase));	\
		return float4(	\
			colour.r,	\
			dot(colour.gb, colourSubcarrier)*0.5 + 0.5,	\
			0.0,	\
			1.0		\
		);	\
	}	\
	\
	fragment float4 compositeSample##name(SourceInterpolator vert [[stage_in]], texture2d<pixelType> texture [[texture(0)]], constant Uniforms &uniforms [[buffer(0)]]) {	\
		const auto colour = uniforms.fromRGB * convert##name(vert, texture);	\
		const float2 colourSubcarrier = float2(cos(vert.colourPhase), sin(vert.colourPhase));	\
		const float level = mix(colour.r, dot(colour.gb, colourSubcarrier), vert.colourAmplitude);	\
		return float4(	\
			level,	\
			0.5 + 0.5*level*cos(vert.colourPhase),\
			0.5 + 0.5*level*sin(vert.colourPhase),\
			1.0		\
		);	\
	}

DeclareShaders(Red8Green8Blue8, float)
DeclareShaders(Red4Green4Blue4, ushort)
DeclareShaders(Red2Green2Blue2, ushort)
DeclareShaders(Red1Green1Blue1, ushort)

// MARK: - Shaders for copying from a same-sized texture to an MTKView's frame buffer.

struct CopyInterpolator {
	float4 position [[position]];
	float2 textureCoordinates;
};

vertex CopyInterpolator copyVertex(uint vertexID [[vertex_id]], texture2d<float> texture [[texture(0)]]) {
	CopyInterpolator vert;

	const uint x = vertexID & 1;
	const uint y = (vertexID >> 1) & 1;

	vert.textureCoordinates = float2(
		x * texture.get_width(),
		y * texture.get_height()
	);
	vert.position = float4(
		float(x) * 2.0 - 1.0,
		1.0 - float(y) * 2.0,
		0.0,
		1.0
	);

	return vert;
}

fragment float4 copyFragment(CopyInterpolator vert [[stage_in]], texture2d<float> texture [[texture(0)]]) {
	return texture.sample(standardSampler, vert.textureCoordinates);
}

fragment float4 clearFragment() {
	return float4(0.0, 0.0, 0.0, 0.64);
}

// MARK: - Conversion fragment shaders

fragment float4 filterFragment(SourceInterpolator vert [[stage_in]], texture2d<float> texture [[texture(0)]], constant Uniforms &uniforms [[buffer(0)]]) {
#define Sample(x)	texture.sample(standardSampler, vert.textureCoordinates + float2(x, 0.0f)).rgb
	const float3 rawSamples[] = {
		Sample(-7),	Sample(-6),	Sample(-5),	Sample(-4),	Sample(-3),	Sample(-2),	Sample(-1),
		Sample(0),
		Sample(1),	Sample(2),	Sample(3),	Sample(4),	Sample(5),	Sample(6),	Sample(7),
	};
#undef Sample

#define Sample(c, o)	\
	uniforms.firCoefficients[c] * rawSamples[o]

	const float3 colour =
		Sample(0, 0) +	Sample(1, 1) +	Sample(2, 2) +	Sample(3, 3) +
		Sample(4, 4) +	Sample(5, 5) +	Sample(6, 6) +
		Sample(7, 7) +
		Sample(6, 8) +	Sample(5, 9) +	Sample(4, 10) +
		Sample(3, 11) +	Sample(2, 12) +	Sample(1, 13) +	Sample(0, 14);

#undef Sample

	return float4(uniforms.toRGB * ((colour - float3(0.0f, 0.5f, 0.5f)) * float3(1.0f, 2.0f / vert.colourAmplitude, 2.0f / vert.colourAmplitude)), 1.0f);
}

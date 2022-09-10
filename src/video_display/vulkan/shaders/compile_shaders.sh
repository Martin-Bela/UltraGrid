#!/bin/bash -x

#set correct glslc location
GLSLC=glslc

SOURCE_PATH=.
DEST_PATH=../../../../share/ultragrid/vulkan_shaders

$GLSLC vulkan_shader.vert -o $DEST_PATH/vert.spv
$GLSLC vulkan_shader.frag -o $DEST_PATH/frag.spv
$GLSLC RGB10A2_conv.comp -o $DEST_PATH/RGB10A2_conv.comp.spv

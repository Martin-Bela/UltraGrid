#!/bin/bash -x

#set correct glslc location
GLSLC=glslc

DEST_PATH=../../../share/ultragrid/vulkan_shaders

$GLSLC vulkan_shader.vert -o $DEST_PATH/vert.spv
$GLSLC vulkan_shader.vert -o $DEST_PATH/vert.spv
$GLSLC identity.comp -o $DEST_PATH/identity.spv

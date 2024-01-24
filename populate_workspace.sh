#!/bin/bash

DIR=`pwd`

TASK0_PROJ_NAME=pipeline_task0
TASK1_PROJ_NAME=pipeline_task1
TASK2_PROJ_NAME=pipeline_task2
TASK3_PROJ_NAME=pipeline_task3

WORKSPACE=$1

ln -s $DIR/TASK0 $WORKSPACE/$TASK0_PROJ_NAME/src/task

ln -s $DIR/TASK1 $WORKSPACE/$TASK1_PROJ_NAME/src/task

ln -s $DIR/TASK2 $WORKSPACE/$TASK2_PROJ_NAME/src/task

ln -s $DIR/TASK3 $WORKSPACE/$TASK3_PROJ_NAME/src/task

ln -s $DIR/hostif $WORKSPACE/$TASK0_PROJ_NAME/src/hostif

ln -s $DIR/hostif $WORKSPACE/$TASK3_PROJ_NAME/src/hostif

ln -s $DIR/config $WORKSPACE/$TASK0_PROJ_NAME/src/config

ln -s $DIR/config $WORKSPACE/$TASK1_PROJ_NAME/src/config

ln -s $DIR/config $WORKSPACE/$TASK2_PROJ_NAME/src/config

ln -s $DIR/config $WORKSPACE/$TASK3_PROJ_NAME/src/config

ln -s $DIR/queue $WORKSPACE/$TASK0_PROJ_NAME/src/queue

ln -s $DIR/queue $WORKSPACE/$TASK1_PROJ_NAME/src/queue

ln -s $DIR/queue $WORKSPACE/$TASK2_PROJ_NAME/src/queue

ln -s $DIR/queue $WORKSPACE/$TASK3_PROJ_NAME/src/queue

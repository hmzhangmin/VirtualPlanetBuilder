/* -*-c++-*- VirtualPlanetBuilder - Copyright (C) 1998-2007 Robert Osfield 
 *
 * This application is open source and may be redistributed and/or modified   
 * freely and without restriction, both in commericial and non commericial applications,
 * as long as this copyright notice is maintained.
 * 
 * This application is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/


#include <vpb/Commandline>
#include <vpb/TaskManager>

#include <osg/Timer>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

#include <iostream>

#include <signal.h>

int main(int argc, char** argv)
{
    osg::ref_ptr<vpb::TaskManager> taskManager = vpb::TaskManager::instance();

    taskManager->setSignalAction(SIGHUP, vpb::TaskManager::COMPLETE_RUNNING_TASKS_THEN_EXIT);
    taskManager->setSignalAction(SIGINT, vpb::TaskManager::TERMINATE_RUNNING_TASKS_THEN_EXIT);
    taskManager->setSignalAction(SIGQUIT, vpb::TaskManager::TERMINATE_RUNNING_TASKS_THEN_EXIT);
    taskManager->setSignalAction(SIGABRT, vpb::TaskManager::TERMINATE_RUNNING_TASKS_THEN_EXIT);
    taskManager->setSignalAction(SIGKILL, vpb::TaskManager::TERMINATE_RUNNING_TASKS_THEN_EXIT);
    taskManager->setSignalAction(SIGTERM, vpb::TaskManager::TERMINATE_RUNNING_TASKS_THEN_EXIT);
    taskManager->setSignalAction(SIGUSR1, vpb::TaskManager::RESET_MACHINE_POOL);
    taskManager->setSignalAction(SIGUSR2, vpb::TaskManager::UPDATE_MACHINE_POOL);

    osg::Timer_t startTick = osg::Timer::instance()->tick();

    osg::ArgumentParser arguments(&argc,argv);

    // set up the usage document, in case we need to print out how to use this program.
    arguments.getApplicationUsage()->setApplicationName(arguments.getApplicationName());
    arguments.getApplicationUsage()->setDescription(arguments.getApplicationName()+" application is utility tools which can be used to generate paged geospatial terrain databases.");
    arguments.getApplicationUsage()->setCommandLineUsage(arguments.getApplicationName()+" [options] filename ...");
    arguments.getApplicationUsage()->addCommandLineOption("-h or --help","Display this information");

    std::string runPath;
    if (arguments.read("--run-path",runPath))
    {
        chdir(runPath.c_str());
    }

    
    taskManager->read(arguments);

    bool buildWithoutSlaves = false;
    while (arguments.read("--build")) { buildWithoutSlaves=true; } 
    
    std::string tasksOutputFileName;
    while (arguments.read("--to",tasksOutputFileName));

    // any option left unread are converted into errors to write out later.
    arguments.reportRemainingOptionsAsUnrecognized();

    // report any errors if they have occured when parsing the program aguments.
    if (arguments.errors())
    {
        arguments.writeErrorMessages(std::cout);
        return 1;
    }
    
    if (!tasksOutputFileName.empty())
    {
        std::string sourceFileName = taskManager->getBuildName() + std::string("_master.source");
        taskManager->writeSource(tasksOutputFileName);

        taskManager->generateTasksFromSource();
        taskManager->writeTasks(tasksOutputFileName);
        return 1;
    }

    if (buildWithoutSlaves)
    {
        taskManager->buildWithoutSlaves();
    }
    else
    {
        if (!taskManager->hasTasks())
        {
            std::string sourceFileName = taskManager->getBuildName() + std::string("_master.source");
            tasksOutputFileName = taskManager->getBuildName() + std::string("_master.tasks");

            taskManager->writeSource(sourceFileName);

            taskManager->generateTasksFromSource();
            taskManager->writeTasks(tasksOutputFileName);
            
            taskManager->log(osg::NOTICE,"Generated tasks file = %s",tasksOutputFileName.c_str());
        }
    
        if (taskManager->hasMachines())
        {
            taskManager->run();
        }
        else
        {
            taskManager->log(osg::NOTICE,"Cannot run build without machines assigned, please pass in a machines definiation file via --machines <file>.");
        }
    }
    
    double duration = osg::Timer::instance()->delta_s(startTick, osg::Timer::instance()->tick());

    taskManager->log(osg::NOTICE,"Total elapsed time = %f",duration);

    return 0;
}

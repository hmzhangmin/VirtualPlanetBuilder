/* -*-c++-*- VirtualPlanetBuilder - Copyright (C) 1998-2007 Robert Osfield 
 *
 * This library is open source and may be redistributed and/or modified under  
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or 
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * OpenSceneGraph Public License for more details.
*/


#include <osg/Texture2D>
#include <osg/ComputeBoundsVisitor>
#include <osg/io_utils>

#include <osg/GLU>

#include <osgTerrain/GeometryTechnique>

#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgDB/FileNameUtils>

#include <osgFX/MultiTextureControl>

#include <osgViewer/GraphicsWindow>
#include <osgViewer/Viewer>
#include <osgViewer/Version>

#include <vpb/DataSet>
#include <vpb/DatabaseBuilder>
#include <vpb/TaskManager>
#include <vpb/System>

#include <vpb/ShapeFilePlacer>

// GDAL includes
#include <gdal_priv.h>
#include <ogr_spatialref.h>

// standard library includes
#include <sstream>
#include <iostream>
#include <algorithm>


using namespace vpb;


DataSet::DataSet()
{
    init();
}

void DataSet::init()
{
    static bool s_initialized = false;
    if (!s_initialized)
    {
        s_initialized = true;
        GDALAllRegister();
    }

    _C1 = 0;
    _R1 = 0;

    _numTextureLevels = 1;
    
    _modelPlacer = new ObjectPlacer;
    _shapeFilePlacer = new ShapeFilePlacer;

}

void DataSet::addSource(Source* source)
{
    if (!source) return;
    
    if (!_sourceGraph.valid()) _sourceGraph = new CompositeSource;
    
    _sourceGraph->_sourceList.push_back(source);
}

void DataSet::addSource(CompositeSource* composite)
{
    if (!composite) return;

    if (!_sourceGraph.valid()) _sourceGraph = new CompositeSource;
    
    _sourceGraph->_children.push_back(composite);
}

void DataSet::loadSources()
{
    assignIntermediateCoordinateSystem();

    FileCache* fileCache = System::instance()->getFileCache();

    for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
    {
        Source* source = itr->get();

        if (source)
        {
            source->loadSourceData();
        
            if (fileCache && source->needReproject(_intermediateCoordinateSystem.get()))
            {
                if (source->isRaster())
                {
                    Source* newSource = source->doRasterReprojectionUsingFileCache(_intermediateCoordinateSystem.get());

                    if (newSource)
                    {
                        *itr = newSource;
                    }
                }
            }
        }
    }
}

bool DataSet::mapLatLongsToXYZ() const
{
    bool result = getConvertFromGeographicToGeocentric() && getEllipsoidModel();
    return result;
}

bool DataSet::computeCoverage(const GeospatialExtents& extents, int level, int& minX, int& minY, int& maxX, int& maxY)
{
    if (!_destinationExtents.intersects(extents)) return false;
    
    if (level==0)
    {
        minX = 0;
        maxX = 1;
        minY = 0;
        maxY = 1;
        return true;
    }

    double destination_xRange = _destinationExtents.xMax()-_destinationExtents.xMin();
    double destination_yRange = _destinationExtents.yMax()-_destinationExtents.yMin();

    int Ck = pow(2.0, double(level-1)) * _C1;
    int Rk = pow(2.0, double(level-1)) * _R1;

    int i_min( floor( ((extents.xMin() - _destinationExtents.xMin()) / destination_xRange) * double(Ck) ) );
    int j_min( floor( ((extents.yMin() - _destinationExtents.yMin()) / destination_yRange) * double(Rk) ) );

    // note i_max and j_max are one beyond the extents required so that the below for loop can use <
    // and the clamping to the 0..Ck-1 and 0..Rk-1 extents will work fine.
    int i_max( ceil( ((extents.xMax() - _destinationExtents.xMin()) / destination_xRange) * double(Ck) ) );
    int j_max( ceil( ((extents.yMax() - _destinationExtents.yMin()) / destination_yRange) * double(Rk) ) );

    // clamp j range to 0 to Ck range
    if (i_min<0) i_min = 0;
    if (i_max<0) i_max = 0;
    if (i_min>Ck) i_min = Ck;
    if (i_max>Ck) i_max = Ck;

    // clamp j range to 0 to Rk range
    if (j_min<0) j_min = 0;
    if (j_max<0) j_max = 0;
    if (j_min>Rk) j_min = Rk;
    if (j_max>Rk) j_max = Rk;
    
    minX = i_min;
    maxX = i_max;
    minY = j_min;
    maxY = j_max;
    
    return (minX<maxX) && (minY<maxY);
}

bool DataSet::computeOptimumLevel(Source* source, int maxLevel, int& level)
{
    if (source->getType()!=Source::IMAGE && source->getType()!=Source::HEIGHT_FIELD) return false;

    if (maxLevel < source->getMinLevel()) return false;

    SourceData* sd = source->getSourceData();
    if (!sd) return false;
    

    const SpatialProperties& sp = sd->computeSpatialProperties(_intermediateCoordinateSystem.get());

    double destination_xRange = _destinationExtents.xMax()-_destinationExtents.xMin();
    double destination_yRange = _destinationExtents.yMax()-_destinationExtents.yMin();

    double source_xRange = sp._extents.xMax()-sp._extents.xMin();
    double source_yRange = sp._extents.yMax()-sp._extents.yMin();

    float sourceResolutionX = (source_xRange)/(float)sp._numValuesX;
    float sourceResolutionY = (source_yRange)/(float)sp._numValuesY;

    log(osg::NOTICE,"Source %s resX %f resY %f",source->getFileName().c_str(), sourceResolutionX, sourceResolutionX);

    double tileSize = source->getType()==Source::IMAGE ? _maximumTileImageSize-2 : _maximumTileTerrainSize-1;

    int k_cols( ceil( 1.0 + ::log( destination_xRange / (_C1 * sourceResolutionX * tileSize ) ) / ::log(2.0) ) );
    int k_rows( ceil( 1.0 + ::log( destination_yRange / (_R1 * sourceResolutionY * tileSize ) ) / ::log(2.0) ) );
    level = std::max(k_cols, k_rows);
    level = std::min(level, int(source->getMaxLevel()));
    level = std::min(level, maxLevel);
    
    return true;
}

bool DataSet::computeOptimumTileSystemDimensions(int& C1, int& R1)
{
    C1 = 1;
    R1 = 1;
    
    double destination_xRange = _destinationExtents.xMax()-_destinationExtents.xMin();
    double destination_yRange = _destinationExtents.yMax()-_destinationExtents.yMin();
    double AR = destination_xRange / destination_yRange;
    
    bool swapAxis = AR<1.0;
    if (swapAxis) AR = 1.0/AR;
    
    double lower_AR = floor(AR);   
    double upper_AR = ceil(AR);
    
    if (AR<sqrt(lower_AR*upper_AR)) 
    {
        C1 = (int)(lower_AR);
        R1 = 1;
    }
    else
    {
        C1 = (int)(upper_AR);
        R1 = 1;
    }
    
    if (swapAxis)
    {
        std::swap(C1,R1);
    }
    
    _C1 = C1;
    _R1 = R1;
    
    log(osg::NOTICE,"AR=%f C1=%i R1=%i",AR,C1,R1);
    
    return true;
}

void DataSet::createNewDestinationGraph(osg::CoordinateSystemNode* cs,
                               const GeospatialExtents& extents,
                               unsigned int maxImageSize,
                               unsigned int maxTerrainSize,
                               unsigned int maxNumLevels)
{
    log(osg::NOTICE,"createNewDestinationGraph");
    
    computeOptimumTileSystemDimensions(_C1,_R1);

    log(osg::NOTICE,"      C1=%i R1=%i",_C1,_R1);
    
    typedef std::list< osg::ref_ptr<Source> > SourceList;
    SourceList modelSources;
    
    for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
    {
        Source* source = (*itr).get();

        if (source->getMinLevel()>maxNumLevels)
        {
            log(osg::NOTICE,"Skipping source %s as its min level excees destination max level.",source->getFileName().c_str());
            continue;
        }

        SourceData* sd = (*itr)->getSourceData();
        if (!sd)
        {
            log(osg::NOTICE,"Skipping source %s as no data loaded from it.",source->getFileName().c_str());
            continue;
        }
        
        const SpatialProperties& sp = sd->computeSpatialProperties(cs);

        if (!sp._extents.intersects(extents))
        {
            // skip this source since it doesn't overlap this tile.
            log(osg::NOTICE,"Skipping source %s as its extents don't overlap destination extents.",source->getFileName().c_str());
            continue;
        }

        double source_xRange = sp._extents.xMax()-sp._extents.xMin();
        double source_yRange = sp._extents.yMax()-sp._extents.yMin();
        
        int level = 0;
        
        if (source->getType()!=Source::IMAGE && source->getType()!=Source::HEIGHT_FIELD)
        {
            // place models and shapefiles into a separate temporary source list and then process these after
            // the main handling of terrain/imagery sources.
            modelSources.push_back(source);
            
            continue;
            
        }
        
        int k = 0;
        if (!computeOptimumLevel(source, maxNumLevels-1, k)) continue;
        
        log(osg::NOTICE,"     opt level = %i",k);

        for(int l=0; l<=k; l++)
        {
            int i_min, i_max, j_min, j_max;
            if (computeCoverage(sp._extents, l, i_min, j_min, i_max, j_max)) 
            {
                log(osg::NOTICE,"     level=%i i_min=%i i_max=%i j_min=%i j_max=%i",l, i_min, i_max, j_min, j_max);

                for(int j=j_min; j<j_max;++j)
                {
                    for(int i=i_min; i<i_max;++i)
                    {
                        if (getComposite(l,i,j)) continue;

                        //log(osg::NOTICE,"       no composite (%i,%i,%i)", i,j,k);
                        CompositeDestination* cd = new CompositeDestination;
                        cd->_level = l;
                        cd->_tileX = i;
                        cd->_tileY = j;
                        insertTileToQuadMap(cd);
                    }
                }

            }
        }

            
            
            

    }
    
    for(SourceList::iterator ml_itr = modelSources.begin();
        ml_itr != modelSources.end();
        ++ml_itr)
    {
        Source* source = (*ml_itr).get();
        SourceData* sd = (*ml_itr)->getSourceData();

        const SpatialProperties& sp = sd->computeSpatialProperties(cs);
        double source_xRange = sp._extents.xMax()-sp._extents.xMin();
        double source_yRange = sp._extents.yMax()-sp._extents.yMin();


        log(osg::NOTICE,"Need to handle shapefile/model source %s.",source->getFileName().c_str());
    }
    
    
}

CompositeDestination* DataSet::createDestinationGraph(CompositeDestination* parent,
                                                      osg::CoordinateSystemNode* cs,
                                                      const GeospatialExtents& extents,
                                                      unsigned int maxImageSize,
                                                      unsigned int maxTerrainSize,
                                                      unsigned int currentLevel,
                                                      unsigned int currentX,
                                                      unsigned int currentY,
                                                      unsigned int maxNumLevels)
{

    if (getGenerateSubtile() && (currentLevel == getSubtileLevel()))
    {
        if ((currentX != getSubtileX()) || (currentY != getSubtileY())) return 0;
    }
    

    CompositeDestination* destinationGraph = new CompositeDestination(cs,extents);

    if (mapLatLongsToXYZ())
    {
        // we need to project the extents into world coords to get the appropriate size to use for control max visible distance
        float max_range = osg::maximum(extents.xMax()-extents.xMin(),extents.yMax()-extents.yMin());
        float projected_radius =  osg::DegreesToRadians(max_range) * getEllipsoidModel()->getRadiusEquator();
        float center_offset = (max_range/360.0f) * getEllipsoidModel()->getRadiusEquator();
        destinationGraph->_maxVisibleDistance = projected_radius * getRadiusToMaxVisibleDistanceRatio() + center_offset;
    }
    else
    {
        destinationGraph->_maxVisibleDistance = extents.radius()*getRadiusToMaxVisibleDistanceRatio();
    }

    // first create the topmost tile

    // create the name
    std::ostringstream os;
    os << _tileBasename << "_L"<<currentLevel<<"_X"<<currentX<<"_Y"<<currentY;

    destinationGraph->_parent = parent;
    destinationGraph->_name = os.str();
    destinationGraph->_level = currentLevel;
    destinationGraph->_tileX = currentX;
    destinationGraph->_tileY = currentY;
    destinationGraph->_dataSet = this;


    DestinationTile* tile = new DestinationTile;
    tile->_name = destinationGraph->_name;
    tile->_level = currentLevel;
    tile->_tileX = currentX;
    tile->_tileY = currentY;
    tile->_dataSet = this;
    tile->_cs = cs;
    tile->_extents = extents;

    // set to NONE as the tile is a mix of RASTER and VECTOR
    // that way the default of RASTER for image and VECTOR for height is maintained
    tile->_dataType = SpatialProperties::NONE;

    tile->_pixelFormat = (getTextureType()==COMPRESSED_RGBA_TEXTURE||
                          getTextureType()==RGBA ||
                          getTextureType()==RGBA_16) ? GL_RGBA : GL_RGB;
    tile->setMaximumImagerySize(maxImageSize,maxImageSize);
    tile->setMaximumTerrainSize(maxTerrainSize,maxTerrainSize);
    tile->computeMaximumSourceResolution(_sourceGraph.get());

    insertTileToQuadMap(destinationGraph);

    if (currentLevel>=maxNumLevels-1 || currentLevel>=tile->_maxSourceLevel)
    {
        // bottom level can't divide any further.
        destinationGraph->_tiles.push_back(tile);
    }
    else
    {
        destinationGraph->_type = LOD;
        destinationGraph->_tiles.push_back(tile);

        bool needToDivideX = false;
        bool needToDivideY = false;

        // note, resolutionSensitivityScale should probably be customizable.. will consider this option for later inclusion.
        double resolutionSensitivityScale = 0.9;
        for(unsigned int layerNum=0;
            layerNum<tile->_imagery.size();
            ++layerNum)
        {
            unsigned int texture_numColumns;
            unsigned int texture_numRows;
            double texture_dx;
            double texture_dy;
            if (tile->computeImageResolution(layerNum,texture_numColumns,texture_numRows,texture_dx,texture_dy))
            {
                if (texture_dx*resolutionSensitivityScale>tile->_imagery[layerNum]._imagery_maxSourceResolutionX) needToDivideX = true;
                if (texture_dy*resolutionSensitivityScale>tile->_imagery[layerNum]._imagery_maxSourceResolutionY) needToDivideY = true;
            }
        }
                
        unsigned int dem_numColumns;
        unsigned int dem_numRows;
        double dem_dx;
        double dem_dy;
        if (tile->computeTerrainResolution(dem_numColumns,dem_numRows,dem_dx,dem_dy))
        {
            if (dem_dx*resolutionSensitivityScale>tile->_terrain_maxSourceResolutionX) needToDivideX = true;
            if (dem_dy*resolutionSensitivityScale>tile->_terrain_maxSourceResolutionY) needToDivideY = true;
        }
        
        float xCenter = (extents.xMin()+extents.xMax())*0.5f;
        float yCenter = (extents.yMin()+extents.yMax())*0.5f;
        
        unsigned int newLevel = currentLevel+1;
        unsigned int newX = currentX*2;
        unsigned int newY = currentY*2;

        if (needToDivideX && needToDivideY)
        {
            float aspectRatio = (extents.yMax()- extents.yMin())/(extents.xMax()- extents.xMin());
            
            if (aspectRatio>1.414) needToDivideX = false;
            else if (aspectRatio<.707) needToDivideY = false;
        }

        if (needToDivideX && needToDivideY)
        {
            log(osg::INFO,"Need to Divide X + Y for level %u",currentLevel);
            // create four tiles.
            GeospatialExtents bottom_left(extents.xMin(),extents.yMin(),xCenter,yCenter, extents._isGeographic);
            GeospatialExtents bottom_right(xCenter,extents.yMin(),extents.xMax(),yCenter, extents._isGeographic);
            GeospatialExtents top_left(extents.xMin(),yCenter,xCenter,extents.yMax(), extents._isGeographic);
            GeospatialExtents top_right(xCenter,yCenter,extents.xMax(),extents.yMax(), extents._isGeographic);

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         bottom_left,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX,
                                                                         newY,
                                                                         maxNumLevels));

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         bottom_right,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX+1,
                                                                         newY,
                                                                         maxNumLevels));

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         top_left,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX,
                                                                         newY+1,
                                                                         maxNumLevels));

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         top_right,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX+1,
                                                                         newY+1,
                                                                         maxNumLevels));
                                                                         
            // Set all there max distance to the same value to ensure the same LOD bining.
            float cutOffDistance = destinationGraph->_maxVisibleDistance*0.5f;
            
            for(CompositeDestination::ChildList::iterator citr=destinationGraph->_children.begin();
                citr!=destinationGraph->_children.end();
                ++citr)
            {
                (*citr)->_maxVisibleDistance = cutOffDistance;
            }

        }
        else if (needToDivideX)
        {
            log(osg::INFO,"Need to Divide X only");

            // create two tiles.
            GeospatialExtents left(extents.xMin(),extents.yMin(),xCenter,extents.yMax(), extents._isGeographic);
            GeospatialExtents right(xCenter,extents.yMin(),extents.xMax(),extents.yMax(), extents._isGeographic);

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         left,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX,
                                                                         newY,
                                                                         maxNumLevels));

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         right,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX+1,
                                                                         newY,
                                                                         maxNumLevels));

                                                                         
            // Set all there max distance to the same value to ensure the same LOD bining.
            float cutOffDistance = destinationGraph->_maxVisibleDistance*0.5f;
            
            for(CompositeDestination::ChildList::iterator citr=destinationGraph->_children.begin();
                citr!=destinationGraph->_children.end();
                ++citr)
            {
                (*citr)->_maxVisibleDistance = cutOffDistance;
            }

        }
        else if (needToDivideY)
        {
            log(osg::INFO,"Need to Divide Y only");

            // create two tiles.
            GeospatialExtents top(extents.xMin(),yCenter,extents.xMax(),extents.yMax(), extents._isGeographic);
            GeospatialExtents bottom(extents.xMin(),extents.yMin(),extents.xMax(),yCenter, extents._isGeographic);

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         bottom,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX,
                                                                         newY,
                                                                         maxNumLevels));

            destinationGraph->addChild(createDestinationGraph(destinationGraph,
                                                                         cs,
                                                                         top,
                                                                         maxImageSize,
                                                                         maxTerrainSize,
                                                                         newLevel,
                                                                         newX,
                                                                         newY+1,
                                                                         maxNumLevels));
                                                                         
            // Set all there max distance to the same value to ensure the same LOD bining.
            float cutOffDistance = destinationGraph->_maxVisibleDistance*0.5f;
            
            for(CompositeDestination::ChildList::iterator citr=destinationGraph->_children.begin();
                citr!=destinationGraph->_children.end();
                ++citr)
            {
                (*citr)->_maxVisibleDistance = cutOffDistance;
            }

        }
        else
        {
            log(osg::INFO,"No Need to Divide");
        }
    }
    
    return destinationGraph;
}

void DataSet::computeDestinationGraphFromSources(unsigned int numLevels)
{
    if (!_sourceGraph) return;

    // ensure we have a valid coordinate system
    if (_destinationCoordinateSystemString.empty()&& !getConvertFromGeographicToGeocentric())
    {
        for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
        {
            SourceData* sd = (*itr)->getSourceData();
            if (sd)
            {
                if (sd->_cs.valid())
                {
                    _destinationCoordinateSystem = sd->_cs;
                    log(osg::INFO,"Setting coordinate system to %s",_destinationCoordinateSystem->getCoordinateSystem().c_str());
                    break;
                }
            }
        }
    }
    
    
    assignIntermediateCoordinateSystem();

    CoordinateSystemType destinateCoordSytemType = getCoordinateSystemType(_destinationCoordinateSystem.get());
    if (destinateCoordSytemType==GEOGRAPHIC && !getConvertFromGeographicToGeocentric())
    {
        // convert elevation into degrees.
        setVerticalScale(1.0f/111319.0f);
    }

    // get the extents of the sources and
    _destinationExtents = _extents;
    _destinationExtents._isGeographic = destinateCoordSytemType==GEOGRAPHIC;

    if (!_destinationExtents.valid()) 
    {
        for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
        {
            SourceData* sd = (*itr)->getSourceData();
            if (sd)
            {
                GeospatialExtents local_extents(sd->getExtents(_intermediateCoordinateSystem.get()));
                log(osg::INFO, "local_extents = xMin() %f %f",local_extents.xMin(),local_extents.xMax());
                log(osg::INFO, "                yMin() %f %f",local_extents.yMin(),local_extents.yMax());
                
                if (destinateCoordSytemType==GEOGRAPHIC)
                {
                    // need to clamp within -180 and 180 range.
                    if (local_extents.xMin()>180.0) 
                    {
                        // shift back to -180 to 180 range
                        local_extents.xMin() -= 360.0;
                        local_extents.xMax() -= 360.0;
                    }
                    else if (local_extents.xMin()<-180.0) 
                    {
                        // shift back to -180 to 180 range
                        local_extents.xMin() += 360.0;
                        local_extents.xMax() += 360.0;
                    }
                }

                _destinationExtents.expandBy(local_extents);
            }
        }
    }
    

    if (destinateCoordSytemType==GEOGRAPHIC)
    {
        double xRange = _destinationExtents.xMax() - _destinationExtents.xMin();
        if (xRange>360.0) 
        {
            // clamp to proper 360 range.
            _destinationExtents.xMin() = -180.0;
            _destinationExtents.xMax() = 180.0;
        }
    }
    

    // compute the number of texture layers required.
    unsigned int maxTextureUnit = 0;
    for(CompositeSource::source_iterator sitr(_sourceGraph.get());sitr.valid();++sitr)
    {
        Source* source = sitr->get();
        if (source) 
        {
            if (maxTextureUnit<source->getLayer()) maxTextureUnit = source->getLayer();
        }
    }
    _numTextureLevels = maxTextureUnit+1;


    log(osg::INFO, "extents = xMin() %f %f",_destinationExtents.xMin(),_destinationExtents.xMax());
    log(osg::INFO, "          yMin() %f %f",_destinationExtents.yMin(),_destinationExtents.yMax());


    osg::Timer_t before = osg::Timer::instance()->tick();

    if (getBuildOptionsString() == "new")
    {
        createNewDestinationGraph(_intermediateCoordinateSystem.get(),
                                  _destinationExtents,
                                  _maximumTileImageSize,
                                  _maximumTileTerrainSize,
                                  numLevels);
    }
    else
    {
        // then create the destination graph accordingly.
        _destinationGraph = createDestinationGraph(0,
                                                   _intermediateCoordinateSystem.get(),
                                                   _destinationExtents,
                                                   _maximumTileImageSize,
                                                   _maximumTileTerrainSize,
                                                   0,
                                                   0,
                                                   0,
                                                   numLevels);
    }
                                                               
    osg::Timer_t after = osg::Timer::instance()->tick();
    
    log(osg::NOTICE,"Time for createDestinationGraph %f", osg::Timer::instance()->delta_s(before, after));


    // now traverse the destination graph to build neighbours.        
    _destinationGraph->computeNeighboursFromQuadMap();

}

void DataSet::assignDestinationCoordinateSystem()
{
    if (getDestinationCoordinateSystem().empty() && !getConvertFromGeographicToGeocentric())
    {
        for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
        {
            Source* source = itr->get();
            source->loadSourceData();
            _destinationCoordinateSystem = source->_cs;
            _destinationCoordinateSystemString = source->_cs.valid() ? source->_cs->getCoordinateSystem() : std::string();

            log(osg::NOTICE,"DataSet::assignDestinationCoordinateSystem() : assigning first source file as the destination coordinate system");
            break;
        }
    }
}


void DataSet::assignIntermediateCoordinateSystem()
{   
    assignDestinationCoordinateSystem();

    if (!_intermediateCoordinateSystem)
    {
        CoordinateSystemType cst = getCoordinateSystemType(_destinationCoordinateSystem.get());

        log(osg::INFO, "new DataSet::createDestination()");
        if (cst!=GEOGRAPHIC && getConvertFromGeographicToGeocentric())
        {
            // need to use the geocentric coordinate system as a base for creating an geographic intermediate
            // coordinate system.
            OGRSpatialReference oSRS;
            
            if (_destinationCoordinateSystem.valid() && !_destinationCoordinateSystem->getCoordinateSystem().empty())
            {
                setIntermediateCoordinateSystem(_destinationCoordinateSystem->getCoordinateSystem());
            }
            else
            {
                char    *pszWKT = NULL;
                oSRS.SetWellKnownGeogCS( "WGS84" );
                oSRS.exportToWkt( &pszWKT );

                setIntermediateCoordinateSystem(pszWKT);                
                setDestinationCoordinateSystem(pszWKT);
            }
            
        }
        else
        {
            _intermediateCoordinateSystem = _destinationCoordinateSystem;
        }
    }
    
}

bool DataSet::requiresReprojection()
{
    assignIntermediateCoordinateSystem();

    loadSources();

    for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
    {
        Source* source = itr->get();
        
        if (source && source->needReproject(_intermediateCoordinateSystem.get()))
        {
            return true;
        }
    }
    return false;
}

void DataSet::updateSourcesForDestinationGraphNeeds()
{
    if (!_destinationGraph || !_sourceGraph) return;


    std::string temporyFilePrefix("temporaryfile_");

    // compute the resolutions of the source that are required.
    {
        _destinationGraph->addRequiredResolutions(_sourceGraph.get());

        for(CompositeSource::source_iterator sitr(_sourceGraph.get());sitr.valid();++sitr)
        {
            Source* source = sitr->get();
            if (source) 
            {
                log(osg::INFO, "Source File %s",source->getFileName().c_str());


                const Source::ResolutionList& resolutions = source->getRequiredResolutions();
                log(osg::INFO, "    resolutions.size() %u",resolutions.size());
                log(osg::INFO, "    { ");
                Source::ResolutionList::const_iterator itr;
                for(itr=resolutions.begin();
                    itr!=resolutions.end();
                    ++itr)
                {
                    log(osg::INFO, "        resX=%f resY=%f",itr->_resX,itr->_resY);
                }
                log(osg::INFO, "    } ");

                source->consolodateRequiredResolutions();

                log(osg::INFO, "    consolodated resolutions.size() %u",resolutions.size());
                log(osg::INFO, "    consolodated { ");
                for(itr=resolutions.begin();
                    itr!=resolutions.end();
                    ++itr)
                {
                    log(osg::INFO, "        resX=%f resY=%f",itr->_resX,itr->_resY);
                }
                log(osg::INFO, "    } ");
            }

        }


    }

    // do standardisation of coordinates systems.
    // do any reprojection if required.
    {
        for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
        {
            Source* source = itr->get();
            
            log(osg::INFO, "Checking %s",source->getFileName().c_str());
            
            if (source && source->needReproject(_intermediateCoordinateSystem.get()))
            {
            
                if (getReprojectSources())
                {
                    if (source->isRaster())
                    {
                
                        // do the reprojection to a tempory file.
                        std::string newFileName = temporyFilePrefix + osgDB::getStrippedName(source->getFileName()) + ".tif";

                        Source* newSource = source->doRasterReprojection(newFileName,_intermediateCoordinateSystem.get());

                        // replace old source by new one.
                        if (newSource) *itr = newSource;
                        else
                        {
                            log(osg::WARN, "Failed to reproject %s",source->getFileName().c_str());
                            *itr = 0;
                        }
                    }
                    else
                    {
                        source->do3DObjectReprojection(_intermediateCoordinateSystem.get());
                    }
                }
                else
                {
                    log(osg::WARN, "Source file %s requires reprojection, but reprojection switched off.",source->getFileName().c_str());
                }
            }
        }
    }
    
    // do sampling of data to required values.
    if (getBuildOverlays())
    {
        for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
        {
            Source* source = itr->get();
            if (source) source->buildOverviews();
        }
    }

    // sort the sources so that the lowest res tiles are drawn first.
    {
        for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
        {
            Source* source = itr->get();
            if (source)
            {
                source->setSortValueFromSourceDataResolution();
                log(osg::INFO, "sort %s value %f",source->getFileName().c_str(),source->getSortValue());
            }
            
        }
        
        // sort them so highest sortValue is first.

        _sourceGraph->sort();
    }
    
    log(osg::INFO, "Using source_lod_iterator itr");
        
    // buggy mips compiler requires this local variable in source_lod_iterator
    // usage below, since using _sourceGraph.get() as it should be was causing
    // a MIPSpro compiler error "The member "vpb::DataSet::_sourceGraph" is inaccessible."
    CompositeSource* my_sourceGraph = _sourceGraph.get();

    for(CompositeSource::source_lod_iterator csitr(my_sourceGraph,CompositeSource::LODSourceAdvancer(0.0));csitr.valid();++csitr)
    {
        Source* source = csitr->get();
        if (source)
        {
            log(osg::INFO, "  LOD %s",(*csitr)->getFileName().c_str());
        }
    }
    log(osg::INFO, "End of Using Source Iterator itr");
    
}

void DataSet::populateDestinationGraphFromSources()
{
    if (!_destinationGraph || !_sourceGraph) return;

    log(osg::NOTICE, "started DataSet::populateDestinationGraphFromSources)");

    if (_databaseType==LOD_DATABASE)
    {

        // for each DestinationTile populate it.
        _destinationGraph->readFrom(_sourceGraph.get());

        // for each DestinationTile equalize the boundaries so they all fit each other without gaps.
        _destinationGraph->equalizeBoundaries();
        
        
    }
    else
    {
        // for each level
        //  compute x and y range
        //  from top row down to bottom row equalize boundairies a write out
    }
    log(osg::NOTICE, "completed DataSet::populateDestinationGraphFromSources)");
}


class ReadFromOperation : public BuildOperation
{
    public:

        ReadFromOperation(ThreadPool* threadPool, BuildLog* buildLog, DestinationTile* tile, CompositeSource* sourceGraph):
            BuildOperation(threadPool, buildLog, "ReadFromOperation", false),
            _tile(tile),
            _sourceGraph(sourceGraph) {}

        virtual void build()
        {
            log(osg::NOTICE, "   ReadFromOperation: reading tile level=%u X=%u Y=%u",_tile->_level,_tile->_tileX,_tile->_tileY);
            _tile->readFrom(_sourceGraph.get());
        }
      
        osg::ref_ptr<DestinationTile> _tile;
        osg::ref_ptr<CompositeSource> _sourceGraph;
};

void DataSet::_readRow(Row& row)
{
    log(osg::NOTICE, "_readRow %u",row.size());
    
    if (_readThreadPool.valid())
    {
        for(Row::iterator citr=row.begin();
            citr!=row.end();
            ++citr)
        {
            CompositeDestination* cd = citr->second;
            for(CompositeDestination::TileList::iterator titr=cd->_tiles.begin();
                titr!=cd->_tiles.end();
                ++titr)
            {
                _readThreadPool->run(new ReadFromOperation(_readThreadPool.get(), getBuildLog(), titr->get(), _sourceGraph.get()));
            }
        }

        // wait for the threads to complete.
        _readThreadPool->waitForCompletion();

    }
    else
    {    
        for(Row::iterator citr=row.begin();
            citr!=row.end();
            ++citr)
        {
            CompositeDestination* cd = citr->second;
            for(CompositeDestination::TileList::iterator titr=cd->_tiles.begin();
                titr!=cd->_tiles.end();
                ++titr)
            {
                DestinationTile* tile = titr->get();
                log(osg::NOTICE, "   reading tile level=%u X=%u Y=%u",tile->_level,tile->_tileX,tile->_tileY);
                tile->readFrom(_sourceGraph.get());
            }
        }
    }
}

void DataSet::_equalizeRow(Row& row)
{
    log(osg::NOTICE, "_equalizeRow %d",row.size());
    for(Row::iterator citr=row.begin();
        citr!=row.end();
        ++citr)
    {
        CompositeDestination* cd = citr->second;
        for(CompositeDestination::TileList::iterator titr=cd->_tiles.begin();
            titr!=cd->_tiles.end();
            ++titr)
        {
            DestinationTile* tile = titr->get();
            log(osg::NOTICE, "   equalizing tile level=%u X=%u Y=%u",tile->_level,tile->_tileX,tile->_tileY);
            tile->equalizeBoundaries();
            tile->setTileComplete(true);
        }
    }
}

void DataSet::_writeNodeFile(const osg::Node& node,const std::string& filename)
{
    if (getDisableWrites()) return;

    if (_archive.valid()) _archive->writeNode(node,filename);
    else osgDB::writeNodeFile(node, filename);
}

void DataSet::_writeImageFile(const osg::Image& image,const std::string& filename)
{
    if (getDisableWrites()) return;

    if (_archive.valid()) _archive->writeImage(image,filename);
    else osgDB::writeImageFile(image, filename);
}


class WriteImageFilesVisitor : public osg::NodeVisitor
{
public:

    WriteImageFilesVisitor(vpb::DataSet* dataSet):
        osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN),
        _dataSet(dataSet) {}

    vpb::DataSet* _dataSet;
    
    virtual void apply(osg::Node& node)
    {
        if (node.getStateSet()) apply(*(node.getStateSet()));

        traverse(node);
    }

    virtual void apply(osg::Group& group)
    {
        osgTerrain::Terrain* terrain = dynamic_cast<osgTerrain::Terrain*>(&group);
        if (terrain)
        {
            applyTerrain(*terrain);
        }
        else
        {
            osg::NodeVisitor::apply(group);
        }
    }

    void applyTerrain(osgTerrain::Terrain& terrain)
    {
        if (terrain.getStateSet()) apply(*(terrain.getStateSet()));
        
        // need to iterator through images stored in layers
    }

    virtual void apply(osg::Geode& geode)
    {
        if (geode.getStateSet()) apply(*(geode.getStateSet()));

        for(unsigned int i=0;i<geode.getNumDrawables();++i)
        {
            if (geode.getDrawable(i)->getStateSet()) apply(*(geode.getDrawable(i)->getStateSet()));
        }
        
        traverse(geode);
    }

    void apply(osg::StateSet& stateset)
    {
        for(unsigned int i=0;i<stateset.getTextureAttributeList().size();++i)
        {
            osg::Image* image = 0;
            osg::Texture2D* texture2D = dynamic_cast<osg::Texture2D*>(stateset.getTextureAttribute(i,osg::StateAttribute::TEXTURE));
            if (texture2D) image = texture2D->getImage();
            
            if (image)
            {
                _dataSet->_writeImageFile(*image,(_dataSet->getDirectory()+image->getFileName()).c_str());
            }
        }
    }
};

class WriteOperation : public BuildOperation
{
    public:

        WriteOperation(ThreadPool* threadPool, DataSet* dataset,CompositeDestination* cd):
            BuildOperation(threadPool, dataset->getBuildLog(), "WriteOperation", false),
            _dataset(dataset),
            _cd(cd) {}

        virtual void build()
        {
            //notify(osg::NOTICE)<<"   WriteOperation"<<std::endl;

            osg::ref_ptr<osg::Node> node = _cd->createSubTileScene();
            std::string filename = _dataset->getDirectory() + _cd->getSubTileName();
            if (node.valid())
            {
                if (_buildLog.valid()) _buildLog->log(osg::NOTICE, "   writeSubTile filename= %s",filename.c_str());
                
                _dataset->_writeNodeFile(*node,filename);

                if (_dataset->getDestinationTileExtension()==".osg")
                {
                    WriteImageFilesVisitor wifv(_dataset);
                    node->accept(wifv);
                }

                _cd->setSubTilesGenerated(true);
                _cd->unrefSubTileData();
            }
            else
            {
                log(osg::WARN, "   failed to writeSubTile node for tile, filename=%s",filename.c_str());
            }
        }
      
        DataSet* _dataset;
        osg::ref_ptr<CompositeDestination> _cd;
};

void DataSet::_writeRow(Row& row)
{
    log(osg::NOTICE, "_writeRow %u",row.size());
    for(Row::iterator citr=row.begin();
        citr!=row.end();
        ++citr)
    {
        CompositeDestination* cd = citr->second;
        CompositeDestination* parent = cd->_parent;
        
        if (parent)
        {
            if (!parent->getSubTilesGenerated() && parent->areSubTilesComplete())
            {
                parent->setSubTilesGenerated(true);
                
                if (_writeThreadPool.valid())
                {
                    _writeThreadPool->run(new WriteOperation(_writeThreadPool.get(), this, parent));
                }
                else
                {
                    osg::ref_ptr<osg::Node> node = parent->createSubTileScene();
                    std::string filename = _directory+parent->getSubTileName();
                    if (node.valid())
                    {
                        log(osg::NOTICE, "   writeSubTile filename= %s",filename.c_str());
                        _writeNodeFile(*node,filename);

                        if (_tileExtension==".osg")
                        {
                            WriteImageFilesVisitor wifv(this);
                            node->accept(wifv);
                        }

                        parent->setSubTilesGenerated(true);
                        parent->unrefSubTileData();

                    }
                    else
                    {
                        log(osg::WARN, "   failed to writeSubTile node for tile, filename=%s",filename.c_str());
                    }
                }
            }
        }
        else
        {
            osg::ref_ptr<osg::Node> node = cd->createPagedLODScene();
            
            if (cd->_level==0)
            {
                if (_decorateWithCoordinateSystemNode)
                {
                    node = decorateWithCoordinateSystemNode(node.get());
                }

                if (_decorateWithMultiTextureControl)
                {
                    node = decorateWithMultiTextureControl(node.get());
                }

                if (!_comment.empty())
                {
                    node->addDescription(_comment);
                }
            }

            std::string filename = _directory+_tileBasename+_tileExtension;

            if (node.valid())
            {
                log(osg::NOTICE, "   writeNodeFile = %u X=%u Y=%u filename=%s",cd->_level,cd->_tileX,cd->_tileY,filename.c_str());
                _writeNodeFile(*node,filename);
                
                if (_tileExtension==".osg")
                {
                    WriteImageFilesVisitor wifv(this);
                    node->accept(wifv);
                }
            }
            else
            {
                log(osg::WARN, "   faild to write node for tile = %u X=%u Y=%u filename=%s",cd->_level,cd->_tileX,cd->_tileY,filename.c_str());
            }

            // record the top nodes as the rootNode of the database
            _rootNode = node;

        }
    }

#if 0
    if (_writeThreadPool.valid()) _writeThreadPool->waitForCompletion();
#endif

}

void DataSet::createDestination(unsigned int numLevels)
{
    log(osg::NOTICE, "started DataSet::createDestination(%u)",numLevels);

    computeDestinationGraphFromSources(numLevels);
    
    updateSourcesForDestinationGraphNeeds();

    log(osg::NOTICE, "completed DataSet::createDestination(%u)",numLevels);

}

osg::Node* DataSet::decorateWithCoordinateSystemNode(osg::Node* subgraph)
{
    // don't decorate if no coord system is set.
    if (!_destinationCoordinateSystem || _destinationCoordinateSystem->getCoordinateSystem().empty()) 
        return subgraph;

    osg::CoordinateSystemNode* csn = new osg::CoordinateSystemNode(
            _destinationCoordinateSystem->getFormat(),
            _destinationCoordinateSystem->getCoordinateSystem());
    
    // set the ellipsoid model if geocentric coords are used.
    if (getConvertFromGeographicToGeocentric()) csn->setEllipsoidModel(getEllipsoidModel());
    
    // add the a subgraph.
    csn->addChild(subgraph);

    return csn;
}

osg::Node* DataSet::decorateWithMultiTextureControl(osg::Node* subgraph)
{
    // if only one layer exists don't need to decorate with MultiTextureControl
    if (_numTextureLevels<=1) return subgraph;
    
    
    // multiple layers active so use osgFX::MultiTextureControl to manage them
    osgFX::MultiTextureControl* mtc = new osgFX::MultiTextureControl;
    float r = 1.0f/(float)_numTextureLevels;
    for(unsigned int i=0;i<_numTextureLevels;++i)
    {
        mtc->setTextureWeight(i,r);
    }

    // add the a subgraph.
    mtc->addChild(subgraph);

    return mtc;
}


void DataSet::_buildDestination(bool writeToDisk)
{
    if (!_state) _state = new osg::State;

    osg::ref_ptr<osgDB::ReaderWriter::Options> previous_options = osgDB::Registry::instance()->getOptions();
    if(previous_options.get())
    {
        log(osg::NOTICE, "vpb: adding optionstring %s",previous_options->getOptionString().c_str());
        osgDB::Registry::instance()->setOptions(new osgDB::ReaderWriter::Options(std::string("precision 16") + std::string(" ") + previous_options->getOptionString()) );
    }
    else 
    {
        osgDB::Registry::instance()->setOptions(new osgDB::ReaderWriter::Options("precision 16"));
    }

    if (!_archive && !_archiveName.empty())
    {
        unsigned int indexBlockSizeHint=4096;
        _archive = osgDB::openArchive(_archiveName, osgDB::Archive::CREATE, indexBlockSizeHint);
    }

    if (_destinationGraph.valid())
    {
        std::string filename = _directory+_tileBasename+_tileExtension;
        
        if (_archive.valid())
        {
            log(osg::NOTICE, "started DataSet::writeDestination(%s)",_archiveName.c_str());
            log(osg::NOTICE, "        archive file = %s",_archiveName.c_str());
            log(osg::NOTICE, "        archive master file = %s",filename.c_str());
        }
        else
        {
            log(osg::NOTICE, "started DataSet::writeDestination(%s)",filename.c_str());
        }

        if (_databaseType==LOD_DATABASE)
        {
            populateDestinationGraphFromSources();
            _rootNode = _destinationGraph->createScene();

            if (_decorateWithCoordinateSystemNode)
            {
                _rootNode = decorateWithCoordinateSystemNode(_rootNode.get());
            }

            if (_decorateWithMultiTextureControl)
            {
                _rootNode = decorateWithMultiTextureControl(_rootNode.get());
            }

            if (!_comment.empty())
            {
                _rootNode->addDescription(_comment);
            }

            if (writeToDisk)
            {
                _writeNodeFile(*_rootNode,filename);
                if (_tileExtension==".osg")
                {
                    WriteImageFilesVisitor wifv(this);
                    _rootNode->accept(wifv);
                }
            }
        }
        else  // _databaseType==PagedLOD_DATABASE
        {

            // for each level build read and write the rows.
            for(QuadMap::iterator qitr=_quadMap.begin();
                qitr!=_quadMap.end();
                ++qitr)
            {
                Level& level = qitr->second;
                
                // skip is level is empty.
                if (level.empty()) continue;
                
                // skip lower levels if we are generating subtiles
                if (getGenerateSubtile() && qitr->first<=getSubtileLevel()) continue;
                
                if (getRecordSubtileFileNamesOnLeafTile() && qitr->first>=getMaximumNumOfLevels()) continue;

                log(osg::INFO, "New level");

                Level::iterator prev_itr = level.begin();
                _readRow(prev_itr->second);
                Level::iterator curr_itr = prev_itr;
                ++curr_itr;
                for(;
                    curr_itr!=level.end();
                    ++curr_itr)
                {
                    _readRow(curr_itr->second);
                    
                    _equalizeRow(prev_itr->second);
                    if (writeToDisk) _writeRow(prev_itr->second);
                    
                    prev_itr = curr_itr;
                }
                
                _equalizeRow(prev_itr->second);
                
                if (writeToDisk)
                {
                    if (writeToDisk) _writeRow(prev_itr->second);
                }

#if 0
                if (_writeThreadPool.valid()) _writeThreadPool->waitForCompletion();
#endif
            }

        }

        if (_archive.valid())
        {
            log(osg::NOTICE, "completed DataSet::writeDestination(%s)",_archiveName.c_str());
            log(osg::NOTICE, "          archive file = %s",_archiveName.c_str());
            log(osg::NOTICE, "          archive master file = %s",filename.c_str());
        }
        else
        {
            log(osg::NOTICE, "completed DataSet::writeDestination(%s)",filename.c_str());
        }

        if (_writeThreadPool.valid()) _writeThreadPool->waitForCompletion();

    }
    else
    {
        log(osg::WARN, "Error: no scene graph to output, no file written.");
    }

    if (_archive.valid()) _archive->close();

    osgDB::Registry::instance()->setOptions(previous_options.get());

}


bool DataSet::addModel(Source::Type type, osg::Node* model)
{
    vpb::Source* source = new vpb::Source(type, model);
    
    osgTerrain::Locator* locator = dynamic_cast<osgTerrain::Locator*>(model->getUserData());
    if (locator)
    {
        osg::notify(osg::NOTICE)<<"addModel : Assigned coordinate system ()"<<std::endl;

        source->setGeoTransformPolicy(vpb::Source::PREFER_CONFIG_SETTINGS);
        source->setGeoTransform(locator->getTransform());

        source->setCoordinateSystemPolicy(vpb::Source::PREFER_CONFIG_SETTINGS);
        source->setCoordinateSystem(locator->getCoordinateSystem());
        
    }
    
    osg::ComputeBoundsVisitor cbv;
    
    
    const osg::Node::DescriptionList& descriptions = model->getDescriptions();
    for(osg::Node::DescriptionList::const_iterator itr = descriptions.begin();
        itr != descriptions.end();
        ++itr)
    {
        int value=0;
        const std::string& desc = *itr;
        if (getAttributeValue(desc, "MinLevel", value))
        {
            source->setMinLevel(value);
        }
        else if (getAttributeValue(desc, "MaxLevel", value))
        {
            source->setMaxLevel(value);
        }
    }
   
    
    model->accept(cbv);
    
    source->_extents.xMin() = cbv.getBoundingBox().xMin();
    source->_extents.xMax() = cbv.getBoundingBox().xMax();

    source->_extents.yMin() = cbv.getBoundingBox().yMin();
    source->_extents.yMax() = cbv.getBoundingBox().yMax();
    
    source->getSourceData()->_extents._min = source->_extents._min;
    source->getSourceData()->_extents._max = source->_extents._max;

    osg::notify(osg::NOTICE)<<"addModel("<<type<<","<<model->getName()<<")"<<std::endl;
    osg::notify(osg::NOTICE)<<"   extents "<<source->_extents.xMin()<<" "<<source->_extents.xMax()<<std::endl;
    osg::notify(osg::NOTICE)<<"           "<<source->_extents.yMin()<<" "<<source->_extents.yMax()<<std::endl;
    
    addSource(source);

    return true;
}

bool DataSet::addLayer(Source::Type type, osgTerrain::Layer* layer, unsigned layerNum)
{
    
    osgTerrain::HeightFieldLayer* hfl = dynamic_cast<osgTerrain::HeightFieldLayer*>(layer);
    if (hfl)
    {
        // need to read locator.
        vpb::Source* source = new vpb::Source(type, hfl->getFileName());
        source->setLayer(layerNum);
        source->setMinLevel(layer->getMinLevel());
        source->setMaxLevel(layer->getMaxLevel());

        if (layer->getLocator() && !layer->getLocator()->getDefinedInFile())
        {
            vpb::Source::ParameterPolicy geoTransformPolicy = layer->getLocator()->getTransformScaledByResolution() ?
                    vpb::Source::PREFER_CONFIG_SETTINGS :
                    vpb::Source::PREFER_CONFIG_SETTINGS_BUT_SCALE_BY_FILE_RESOLUTION;

            source->setGeoTransformPolicy(geoTransformPolicy);
            source->setGeoTransform(layer->getLocator()->getTransform());

            source->setCoordinateSystemPolicy(vpb::Source::PREFER_CONFIG_SETTINGS);
            source->setCoordinateSystem(layer->getLocator()->getCoordinateSystem());
        } 

        addSource(source);
        return true;
    }
    
    osgTerrain::ImageLayer* iml = dynamic_cast<osgTerrain::ImageLayer*>(layer);
    if (iml)
    {
        // need to read locator
        vpb::Source* source = new vpb::Source(type, iml->getFileName());
        source->setLayer(layerNum);
        source->setMinLevel(layer->getMinLevel());
        source->setMaxLevel(layer->getMaxLevel());

        if (layer->getLocator() && !layer->getLocator()->getDefinedInFile())
        {
            vpb::Source::ParameterPolicy geoTransformPolicy = layer->getLocator()->getTransformScaledByResolution() ?
                    vpb::Source::PREFER_CONFIG_SETTINGS :
                    vpb::Source::PREFER_CONFIG_SETTINGS_BUT_SCALE_BY_FILE_RESOLUTION;

            source->setGeoTransformPolicy(geoTransformPolicy);
            source->setGeoTransform(layer->getLocator()->getTransform());

            source->setCoordinateSystemPolicy(vpb::Source::PREFER_CONFIG_SETTINGS);
            source->setCoordinateSystem(layer->getLocator()->getCoordinateSystem());
        }

        addSource(source);
        return true;
    }
    
    osgTerrain::ProxyLayer* pl = dynamic_cast<osgTerrain::ProxyLayer*>(layer);
    if (pl)
    {
        // remove any implementation as we don't need it.
        pl->setImplementation(0);

        vpb::Source* source = new vpb::Source(type, pl->getFileName());
        source->setLayer(layerNum);
        source->setMinLevel(layer->getMinLevel());
        source->setMaxLevel(layer->getMaxLevel());

        if (layer->getLocator() && !layer->getLocator()->getDefinedInFile())
        {
            vpb::Source::ParameterPolicy geoTransformPolicy = layer->getLocator()->getTransformScaledByResolution() ?
                    vpb::Source::PREFER_CONFIG_SETTINGS :
                    vpb::Source::PREFER_CONFIG_SETTINGS_BUT_SCALE_BY_FILE_RESOLUTION;

            source->setGeoTransformPolicy(geoTransformPolicy);
            source->setGeoTransform(layer->getLocator()->getTransform());

            source->setCoordinateSystemPolicy(vpb::Source::PREFER_CONFIG_SETTINGS);
            source->setCoordinateSystem(layer->getLocator()->getCoordinateSystem());
        }

        addSource(source);
        return true;
    }

    osgTerrain::CompositeLayer* compositeLayer = dynamic_cast<osgTerrain::CompositeLayer*>(layer);
    {
        for(unsigned int i=0; i<compositeLayer->getNumLayers();++i)
        {
            if (compositeLayer->getLayer(i))
            {
                addLayer(type, compositeLayer->getLayer(i), layerNum);
            }
            else if (!compositeLayer->getFileName(i).empty())
            {
                vpb::Source* source = new vpb::Source(type, compositeLayer->getFileName(i));
                source->setMinLevel(layer->getMinLevel());
                source->setMaxLevel(layer->getMaxLevel());
                source->setLayer(layerNum);
                addSource(source);
            }
        }
        return true;
    }
    return false;
}

bool DataSet::addTerrain(osgTerrain::Terrain* terrain)
{
    log(osg::NOTICE,"Adding terrain %s",terrain->getName().c_str());

    if (terrain->getLocator())
    {
    }
 
    vpb::DatabaseBuilder* db = dynamic_cast<vpb::DatabaseBuilder*>(terrain->getTerrainTechnique());
    if (db && db->getBuildOptions())
    {
        setBuildOptions(*(db->getBuildOptions()));
    }

    if (terrain->getElevationLayer())
    {
        addLayer(vpb::Source::HEIGHT_FIELD, terrain->getElevationLayer(), 0);
    }

    for(unsigned int i=0; i<terrain->getNumColorLayers();++i)
    {
        osgTerrain::Layer* layer = terrain->getColorLayer(i);
        if (layer) 
        {
            addLayer(vpb::Source::IMAGE, layer, i);
        }
    }
    
    for(unsigned int ci=0; ci<terrain->getNumChildren(); ++ci)
    {
    
        osg::Node* model = terrain->getChild(ci);
    
        osg::notify(osg::NOTICE)<<"Adding model"<<model->getName()<<std::endl;

        Source::Type type = vpb::Source::MODEL;
        for(unsigned di = 0; di< model->getNumDescriptions(); ++di)
        {
            const std::string& desc = model->getDescription(di);
            if (desc=="SHAPEFILE") type = Source::SHAPEFILE;
        }
        
        addModel(type, model);
    }

    return true;
}

osgTerrain::Terrain* DataSet::createTerrainRepresentation()
{
    osg::ref_ptr<osgTerrain::Terrain> terrain = new osgTerrain::Terrain;

    for(CompositeSource::source_iterator itr(_sourceGraph.get());itr.valid();++itr)
    {
        osg::ref_ptr<Source> source = (*itr);
#if 0
            osg::Locator* locator = new osg::Locator;
            osg::ref_ptr<osg::CoordinateSystemNode>     _cs;
            osg::Matrixd                                _geoTransform;
            GeospatialExtents                           _extents;
            DataType                                    _dataType;
            unsigned int                                _numValuesX;
            unsigned int                                _numValuesY;
            unsigned int                                _numValuesZ;
#endif
        unsigned int layerNum = source->getLayer();


        osg::ref_ptr<osg::Object> loadedObject = osgDB::readObjectFile(source->getFileName()+".gdal");
        osgTerrain::Layer* loadedLayer = dynamic_cast<osgTerrain::Layer*>(loadedObject.get());

        if (loadedLayer)
        {
            if (!loadedLayer->getLocator())
            {
                osgTerrain::Locator* locator = new osgTerrain::Locator;
                locator->setTransform(source->getGeoTransform());
                
                if (source->_cs.valid())
                {
                    locator->setCoordinateSystem(source->_cs->getCoordinateSystem());
                    locator->setFormat(source->_cs->getFormat());
                    locator->setEllipsoidModel(source->_cs->getEllipsoidModel());

                    switch(getCoordinateSystemType(source->_cs.get()))
                    {
                        case(GEOCENTRIC): locator->setCoordinateSystemType(osgTerrain::Locator::GEOCENTRIC); break;
                        case(GEOGRAPHIC): locator->setCoordinateSystemType(osgTerrain::Locator::GEOGRAPHIC); break;
                        case(PROJECTED): locator->setCoordinateSystemType(osgTerrain::Locator::PROJECTED); break;
                        case(LOCAL): locator->setCoordinateSystemType(osgTerrain::Locator::PROJECTED); break;
                    };
                }

                loadedLayer->setLocator(locator);
            }
        
            if (source->getType()==Source::IMAGE)
            {
                osgTerrain::Layer* existingLayer = (layerNum < terrain->getNumColorLayers()) ? terrain->getColorLayer(layerNum) : 0;
                osgTerrain::CompositeLayer* compositeLayer = dynamic_cast<osgTerrain::CompositeLayer*>(existingLayer);

                if (compositeLayer)
                {
                    compositeLayer->addLayer( loadedLayer );
                }
                else if (existingLayer)
                {
                    compositeLayer = new osgTerrain::CompositeLayer;
                    compositeLayer->addLayer( existingLayer );
                    compositeLayer->addLayer( loadedLayer );

                    terrain->setColorLayer(layerNum, compositeLayer);
                }
                else
                {
                    terrain->setColorLayer(layerNum, loadedLayer);
                }
            }
            else if (source->getType()==Source::HEIGHT_FIELD)
            {
                osgTerrain::Layer* existingLayer = terrain->getElevationLayer();
                osgTerrain::CompositeLayer* compositeLayer = dynamic_cast<osgTerrain::CompositeLayer*>(existingLayer);

                if (compositeLayer)
                {
                    compositeLayer->addLayer( loadedLayer );
                }
                else if (existingLayer)
                {
                    compositeLayer = new osgTerrain::CompositeLayer;
                    compositeLayer->addLayer( existingLayer );
                    compositeLayer->addLayer( loadedLayer );

                    terrain->setElevationLayer(compositeLayer);
                }
                else
                {
                    terrain->setElevationLayer(loadedLayer);
                }
            }
        }
    }
    
    osg::ref_ptr<DatabaseBuilder> builder = new DatabaseBuilder;
    builder->setBuildOptions(new BuildOptions(*this));
    builder->setBuildLog(getBuildLog());
    terrain->setTerrainTechnique(builder.get());

    return terrain.release();
}

class MyGraphicsContext : public osg::Referenced {
    public:
        MyGraphicsContext(BuildLog* buildLog)
        {
            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            traits->x = 0;
            traits->y = 0;
            traits->width = 1;
            traits->height = 1;
            traits->windowDecoration = false;
            traits->doubleBuffer = false;
            traits->sharedContext = 0;
            traits->pbuffer = true;

            _gc = osg::GraphicsContext::createGraphicsContext(traits.get());

            if (!_gc)
            {
                if (buildLog) buildLog->log(osg::NOTICE,"Failed to create pbuffer, failing back to normal graphics window.");
                
                traits->pbuffer = false;
                _gc = osg::GraphicsContext::createGraphicsContext(traits.get());
            }

            if (_gc.valid()) 
            
            
            {
                _gc->realize();
                _gc->makeCurrent();
                
                if (buildLog) buildLog->log(osg::NOTICE,"Realized window");
            }
        }
        
        bool valid() const { return _gc.valid() && _gc->isRealized(); }
        
    private:
        osg::ref_ptr<osg::GraphicsContext> _gc;
};


class CollectSubtiles : public DestinationVisitor
{
public:

    CollectSubtiles(unsigned int level):
        _level(level) {}

    virtual void apply(CompositeDestination& cd)
    {
        if (cd._level<_level) traverse(cd);
        else if (cd._level==_level) _subtileList.push_back(&cd);
    }

    typedef std::list< osg::ref_ptr<CompositeDestination> > SubtileList;
    unsigned int    _level;
    SubtileList     _subtileList;

};

bool DataSet::generateTasks(TaskManager* taskManager)
{
    if (getDistributedBuildSplitLevel()==0) return false;

    if (!getLogFileName().empty())
    {
        if (!getBuildLog()) setBuildLog(new BuildLog());
    
        getBuildLog()->openLogFile(getLogFileName());
    }

    if (getBuildLog())
    {
        pushOperationLog(getBuildLog());
    }
    
    loadSources();

    computeDestinationGraphFromSources(getDistributedBuildSplitLevel()+1);

    if (_destinationGraph.valid())
    {
        CollectSubtiles cs(getDistributedBuildSplitLevel()-1);
        
        _destinationGraph->accept(cs);
        
        std::string sourceFile = taskManager->getSourceFileName();
        
        std::string basename = taskManager->getBuildName();
        std::string taskDirectory = getTaskDirectory();
        if (!taskDirectory.empty()) taskDirectory += "/";
        
        std::string fileCacheName;
        if (System::instance()->getFileCache()) fileCacheName = System::instance()->getFileCache()->getFileName(); 
        
        bool logging = getNotifyLevel() > ALWAYS;
        
        // create root task
        {
            std::ostringstream taskfile;
            taskfile<<taskDirectory<<basename<<"_root_L0_X0_Y0.task";

            std::ostringstream app;
            app<<"osgdem --run-path "<<taskManager->getRunPath()<<" -s "<<sourceFile<<" --record-subtile-on-leaf-tiles -l "<<getDistributedBuildSplitLevel()<<" --task "<<taskfile.str();

            if (!fileCacheName.empty())
            {
                app<<" --cache "<<fileCacheName;
            }

            if (logging)
            {
                std::ostringstream logfile;
                logfile<<taskDirectory<<basename<<"_root_L0_X0_Y0.log";
                app<<" --log "<<logfile.str();
            }
#if 0            
            else
            {
                app<<" > /dev/null";
            }
#endif            

            taskManager->addTask(taskfile.str(), app.str(), sourceFile);
        }
        
        // taskManager->nextTaskSet();
        
        for(CollectSubtiles::SubtileList::iterator itr = cs._subtileList.begin();
            itr != cs._subtileList.end();
            ++itr)
        {
            CompositeDestination* cd = itr->get();
            
            std::ostringstream taskfile;
            taskfile<<taskDirectory<<basename<<"_subtile_L"<<cd->_level<<"_X"<<cd->_tileX<<"_Y"<<cd->_tileY<<".task";


            std::ostringstream app;
            app<<"osgdem --run-path "<<taskManager->getRunPath()<<" -s "<<sourceFile<<" --subtile "<<cd->_level<<" "<<cd->_tileX<<" "<<cd->_tileY<<" --task "<<taskfile.str();

            if (!fileCacheName.empty())
            {
                app<<" --cache "<<fileCacheName;
            }

            if (logging)
            {
                std::ostringstream logfile;

                logfile<<taskDirectory<<basename<<"_subtile_L"<<cd->_level<<"_X"<<cd->_tileX<<"_Y"<<cd->_tileY<<".log";
                app<<" --log "<<logfile.str();
            }
#if 0
            else
            {
                app<<" > /dev/null";
            }
#endif
            taskManager->addTask(taskfile.str(), app.str(), sourceFile);
        }

    }

    if (getBuildLog())
    {
        popOperationLog();
    }
    
    return true;
}

int DataSet::run()
{
    if (!getLogFileName().empty())
    {
        if (!getBuildLog()) setBuildLog(new BuildLog());
        
        getBuildLog()->openLogFile(getLogFileName());
    }

    if (getBuildLog())
    {
        pushOperationLog(getBuildLog());
    }
    
    bool requiresGraphicsContextInMainThread = true;
    
    int numProcessors = OpenThreads::GetNumberOfProcessors();
#if 0    
    if (numProcessors>1)
#endif
    {
        int numReadThreads = int(ceilf(getNumReadThreadsToCoresRatio() * float(numProcessors)));
        if (numReadThreads>=1)
        {
            log(osg::NOTICE,"Starting %i read threads.",numReadThreads);
            _readThreadPool = new ThreadPool(numReadThreads, false);
            _readThreadPool->startThreads();
        }
        
        int numWriteThreads = int(ceilf(getNumWriteThreadsToCoresRatio() * float(numProcessors)));
        if (numWriteThreads>=1)
        {
            log(osg::NOTICE,"Starting %i write threads.",numWriteThreads);
            _writeThreadPool = new ThreadPool(numWriteThreads, true);
            _writeThreadPool->startThreads();
            
            //requiresGraphicsContextInMainThread = false;
        }
    }
    

    loadSources();

    int numLevels = getMaximumNumOfLevels();
    if (getRecordSubtileFileNamesOnLeafTile()) ++numLevels;

    createDestination(numLevels);
    
    bool requiresGenerationOfTiles = getGenerateTiles();
    
    if (!getIntermediateBuildName().empty())
    {
        osg::ref_ptr<osgTerrain::Terrain> terrain = createTerrainRepresentation();
        if (terrain.valid())
        {
            DatabaseBuilder* db = dynamic_cast<DatabaseBuilder*>(terrain->getTerrainTechnique());
            if (db && db->getBuildOptions()) 
            {
                db->getBuildOptions()->setIntermediateBuildName("");
            }
            osgDB::writeNodeFile(*terrain,getIntermediateBuildName());
            requiresGenerationOfTiles = false;
        }
    }

    if (requiresGenerationOfTiles)
    {
        if (getTextureType()==COMPRESSED_TEXTURE || getTextureType()==COMPRESSED_RGBA_TEXTURE)
        {
            // dummy Viewer to get round silly Windows autoregistration problem for GraphicsWindowWin32.cpp
            osgViewer::Viewer viewer;

            osg::ref_ptr<MyGraphicsContext> context;
            
            if (requiresGraphicsContextInMainThread)
            {
                context  = new MyGraphicsContext(getBuildLog());
                if (!context || !context->valid())
                {
                    log(osg::NOTICE,"Error: Unable to create graphis context, problem with running osgViewer-%s, cannot run compression.",osgViewerGetVersion());
                    return 1;
                }
            }

            writeDestination();
        }
        else
        {
            writeDestination();
        }
    }

    if (getBuildLog())
    {
        popOperationLog();
    }

    return true;
}

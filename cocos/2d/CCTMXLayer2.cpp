/****************************************************************************
Copyright (c) 2008-2010 Ricardo Quesada
Copyright (c) 2010-2012 cocos2d-x.org
Copyright (c) 2011      Zynga Inc.
Copyright (c) 2013-2014 Chukong Technologies Inc.

http://www.cocos2d-x.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/
#include "CCTMXLayer2.h"
#include "CCTMXXMLParser.h"
#include "CCTMXTiledMap.h"
#include "CCSprite.h"
#include "CCTextureCache.h"
#include "CCShaderCache.h"
#include "CCGLProgram.h"
#include "ccCArray.h"
#include "CCDirector.h"
#include "renderer/CCRenderer.h"

NS_CC_BEGIN


// TMXLayer2 - init & alloc & dealloc

TMXLayer2 * TMXLayer2::create(TMXTilesetInfo *tilesetInfo, TMXLayerInfo *layerInfo, TMXMapInfo *mapInfo)
{
    TMXLayer2 *ret = new TMXLayer2();
    if (ret->initWithTilesetInfo(tilesetInfo, layerInfo, mapInfo))
    {
        ret->autorelease();
        return ret;
    }
    return nullptr;
}
bool TMXLayer2::initWithTilesetInfo(TMXTilesetInfo *tilesetInfo, TMXLayerInfo *layerInfo, TMXMapInfo *mapInfo)
{    
    // XXX: is 35% a good estimate ?
    Size size = layerInfo->_layerSize;
    int totalNumberOfTiles = size.width * size.height;

    Texture2D *texture = nullptr;
    if( tilesetInfo )
    {
        texture = Director::getInstance()->getTextureCache()->addImage(tilesetInfo->_sourceImage.c_str());
        texture->retain();
    }

    _texture = texture;

    // layerInfo
    _layerName = layerInfo->_name;
    _layerSize = size;
    _tiles = layerInfo->_tiles;
    _opacity = layerInfo->_opacity;
    setProperties(layerInfo->getProperties());
    _contentScaleFactor = Director::getInstance()->getContentScaleFactor(); 

    // tilesetInfo
    _tileSet = tilesetInfo;
    CC_SAFE_RETAIN(_tileSet);

    // mapInfo
    _mapTileSize = mapInfo->getTileSize();
    _layerOrientation = mapInfo->getOrientation();

    // offset (after layer orientation is set);
    Point offset = this->calculateLayerOffset(layerInfo->_offset);
    this->setPosition(CC_POINT_PIXELS_TO_POINTS(offset));

    _atlasIndexArray = ccCArrayNew(totalNumberOfTiles);

    this->setContentSize(CC_SIZE_PIXELS_TO_POINTS(Size(_layerSize.width * _mapTileSize.width, _layerSize.height * _mapTileSize.height)));

    _useAutomaticVertexZ = false;
    _vertexZvalue = 0;

    _allQuads = (struct V3F_C4B_T2F_Quad*)malloc( sizeof(_allQuads[0]) * totalNumberOfTiles );
    _quadsToRender = (struct V3F_C4B_T2F_Quad*)malloc( sizeof(_allQuads[0]) * totalNumberOfTiles );
    _indices = (GLushort *)malloc( totalNumberOfTiles * 6 * sizeof(GLushort) );

    // shader, and other stuff
    setShaderProgram(ShaderCache::getInstance()->getProgram(GLProgram::SHADER_NAME_POSITION_TEXTURE_COLOR));


    return true;
}

TMXLayer2::TMXLayer2()
:_layerName("")
,_opacity(0)
,_vertexZvalue(0)
,_useAutomaticVertexZ(false)
,_atlasIndexArray(nullptr)
,_contentScaleFactor(1.0f)
,_layerSize(Size::ZERO)
,_mapTileSize(Size::ZERO)
,_tiles(nullptr)
,_tileSet(nullptr)
,_layerOrientation(TMXOrientationOrtho)
,_reusedTile(nullptr)
{}

TMXLayer2::~TMXLayer2()
{
    free(_allQuads);
    free(_quadsToRender);
    CC_SAFE_RELEASE(_tileSet);
    CC_SAFE_RELEASE(_texture);

    if (_atlasIndexArray)
    {
        ccCArrayFree(_atlasIndexArray);
        _atlasIndexArray = nullptr;
    }

    CC_SAFE_DELETE_ARRAY(_tiles);
}

void TMXLayer2::draw()
{
    _customCommand.init(_globalZOrder);
    _customCommand.func = CC_CALLBACK_0(TMXLayer2::onDraw, this);
    Director::getInstance()->getRenderer()->addCommand(&_customCommand);
}

void TMXLayer2::onDraw()
{
    //
    // Using VBO without VAO
    //

    CC_NODE_DRAW_SETUP();

    GL::enableVertexAttribs(GL::VERTEX_ATTRIB_FLAG_POS_COLOR_TEX );

    GL::bindTexture2D( _texture->getName() );

#define kQuadSize sizeof(_allQuads[0].bl)
    glBindBuffer(GL_ARRAY_BUFFER, _buffersVBO[0]);

    // XXX: update is done in draw... perhaps it should be done in a timer
    glBufferData(GL_ARRAY_BUFFER, sizeof(_allQuads[0]) * _totalTiles, _allQuads, GL_DYNAMIC_DRAW);

    GL::enableVertexAttribs(GL::VERTEX_ATTRIB_FLAG_POS_COLOR_TEX);

    // vertices
    glVertexAttribPointer(GLProgram::VERTEX_ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE, kQuadSize, (GLvoid*) offsetof(V3F_C4B_T2F, vertices));

    // colors
    glVertexAttribPointer(GLProgram::VERTEX_ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, kQuadSize, (GLvoid*) offsetof(V3F_C4B_T2F, colors));

    // tex coords
    glVertexAttribPointer(GLProgram::VERTEX_ATTRIB_TEX_COORDS, 2, GL_FLOAT, GL_FALSE, kQuadSize, (GLvoid*) offsetof(V3F_C4B_T2F, texCoords));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _buffersVBO[1]);

    glDrawElements(GL_TRIANGLES, (GLsizei)_totalTiles*6, GL_UNSIGNED_SHORT, 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void TMXLayer2::setupIndices()
{
    for( int i=0; i < _totalTiles; i++)
    {
        _indices[i*6+0] = i*4+0;
        _indices[i*6+1] = i*4+1;
        _indices[i*6+2] = i*4+2;

        // inverted index. issue #179
        _indices[i*6+3] = i*4+3;
        _indices[i*6+4] = i*4+2;
        _indices[i*6+5] = i*4+1;        
    }
}

void TMXLayer2::setupVBO()
{
    glGenBuffers(2, &_buffersVBO[0]);

    glBindBuffer(GL_ARRAY_BUFFER, _buffersVBO[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(_allQuads[0]) * _totalTiles, _allQuads, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _buffersVBO[1]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(_indices[0]) * _totalTiles * 6, _indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    CHECK_GL_ERROR_DEBUG();
}

void TMXLayer2::releaseMap()
{
    if (_tiles)
    {
        delete [] _tiles;
        _tiles = nullptr;
    }

    if (_atlasIndexArray)
    {
        ccCArrayFree(_atlasIndexArray);
        _atlasIndexArray = nullptr;
    }
}

// TMXLayer2 - setup Tiles
void TMXLayer2::setupTiles()
{    
    // Optimization: quick hack that sets the image size on the tileset
    _tileSet->_imageSize = _texture->getContentSizeInPixels();

    // By default all the tiles are aliased
    // pros:
    //  - easier to render
    // cons:
    //  - difficult to scale / rotate / etc.
    _texture->setAliasTexParameters();

    //CFByteOrder o = CFByteOrderGetCurrent();

    // Parse cocos2d properties
    this->parseInternalProperties();

    for (int y=0; y < _layerSize.height; y++)
    {
        for (int x=0; x < _layerSize.width; x++)
        {
            int pos = static_cast<int>(x + _layerSize.width * y);
            int gid = _tiles[ pos ];

            // gid are stored in little endian.
//            gid = ntohl(gid);

            // XXX: gid == 0 --> empty tile
            if (gid != 0) 
            {
                this->appendTileForGID(gid, Point(x, y));
            }
        }
    }

    _totalTiles = _layerSize.height * _layerSize.width;

    setupIndices();
    setupVBO();
}

// used only when parsing the map. useless after the map was parsed
// since lot's of assumptions are no longer true
void TMXLayer2::appendTileForGID(int gid, const Point& pos)
{
    if (gid != 0 && (static_cast<int>((gid & kFlippedMask)) - _tileSet->_firstGid) >= 0)
    {
        Rect rect = _tileSet->rectForGID(gid);
        rect = CC_RECT_PIXELS_TO_POINTS(rect);

        ssize_t z = (ssize_t)(pos.x + pos.y * _layerSize.width);

        Sprite *sprite = reusedTileWithRect(rect);

        setupTileSprite(sprite ,pos ,gid);

        // don't add it using the "standard" way.
        sprite->setDirty(true);
        sprite->updateTransform();
        V3F_C4B_T2F_Quad quad = sprite->getQuad();

        _allQuads[z] = quad;
    }
}

Sprite* TMXLayer2::reusedTileWithRect(Rect rect)
{
    if (! _reusedTile)
    {
        _reusedTile = Sprite::createWithTexture(_texture, rect);
        _reusedTile->retain();
    }
    else
    {
		// Re-init the sprite
        _reusedTile->setTextureRect(rect, false, rect.size);
    }

    return _reusedTile;
}

void TMXLayer2::setupTileSprite(Sprite* sprite, Point pos, int gid)
{
    sprite->setPosition(getPositionAt(pos));
    sprite->setVertexZ((float)getVertexZForPos(pos));
    sprite->setAnchorPoint(Point::ZERO);
    sprite->setOpacity(_opacity);

    //issue 1264, flip can be undone as well
    sprite->setFlippedX(false);
    sprite->setFlippedY(false);
    sprite->setRotation(0.0f);
    sprite->setAnchorPoint(Point(0,0));

    // Rotation in tiled is achieved using 3 flipped states, flipping across the horizontal, vertical, and diagonal axes of the tiles.
    if (gid & kTMXTileDiagonalFlag)
    {
        // put the anchor in the middle for ease of rotation.
        sprite->setAnchorPoint(Point(0.5f,0.5f));
        sprite->setPosition(Point(getPositionAt(pos).x + sprite->getContentSize().height/2,
                                  getPositionAt(pos).y + sprite->getContentSize().width/2 ) );

        int flag = gid & (kTMXTileHorizontalFlag | kTMXTileVerticalFlag );

        // handle the 4 diagonally flipped states.
        if (flag == kTMXTileHorizontalFlag)
        {
            sprite->setRotation(90.0f);
        }
        else if (flag == kTMXTileVerticalFlag)
        {
            sprite->setRotation(270.0f);
        }
        else if (flag == (kTMXTileVerticalFlag | kTMXTileHorizontalFlag) )
        {
            sprite->setRotation(90.0f);
            sprite->setFlippedX(true);
        }
        else
        {
            sprite->setRotation(270.0f);
            sprite->setFlippedX(true);
        }
    }
    else
    {
        if (gid & kTMXTileHorizontalFlag)
        {
            sprite->setFlippedX(true);
        }

        if (gid & kTMXTileVerticalFlag)
        {
            sprite->setFlippedY(true);
        }
    }
}

// TMXLayer2 - Properties
Value TMXLayer2::getProperty(const std::string& propertyName) const
{
    if (_properties.find(propertyName) != _properties.end())
        return _properties.at(propertyName);
    
    return Value();
}

void TMXLayer2::parseInternalProperties()
{
    // if cc_vertex=automatic, then tiles will be rendered using vertexz

    auto vertexz = getProperty("cc_vertexz");
    if (!vertexz.isNull())
    {
        std::string vertexZStr = vertexz.asString();
        // If "automatic" is on, then parse the "cc_alpha_func" too
        if (vertexZStr == "automatic")
        {
            _useAutomaticVertexZ = true;
            auto alphaFuncVal = getProperty("cc_alpha_func");
            float alphaFuncValue = alphaFuncVal.asFloat();
            setShaderProgram(ShaderCache::getInstance()->getProgram(GLProgram::SHADER_NAME_POSITION_TEXTURE_ALPHA_TEST));

            GLint alphaValueLocation = glGetUniformLocation(getShaderProgram()->getProgram(), GLProgram::UNIFORM_NAME_ALPHA_TEST_VALUE);

            // NOTE: alpha test shader is hard-coded to use the equivalent of a glAlphaFunc(GL_GREATER) comparison
            
            // use shader program to set uniform
            getShaderProgram()->use();
            getShaderProgram()->setUniformLocationWith1f(alphaValueLocation, alphaFuncValue);
            CHECK_GL_ERROR_DEBUG();
        }
        else
        {
            _vertexZvalue = vertexz.asInt();
        }
    }
}


int TMXLayer2::getTileGIDAt(const Point& pos, ccTMXTileFlags* flags/* = nullptr*/)
{
    CCASSERT(pos.x < _layerSize.width && pos.y < _layerSize.height && pos.x >=0 && pos.y >=0, "TMXLayer2: invalid position");
    CCASSERT(_tiles && _atlasIndexArray, "TMXLayer2: the tiles map has been released");

    int idx = static_cast<int>((pos.x + pos.y * _layerSize.width));
    // Bits on the far end of the 32-bit global tile ID are used for tile flags
    int tile = _tiles[idx];

    // issue1264, flipped tiles can be changed dynamically
    if (flags) 
    {
        *flags = (ccTMXTileFlags)(tile & kFlipedAll);
    }
    
    return (tile & kFlippedMask);
}

// TMXLayer2 - atlasIndex and Z
static inline int compareInts(const void * a, const void * b)
{
    return ((*(int*)a) - (*(int*)b));
}

ssize_t TMXLayer2::atlasIndexForExistantZ(int z)
{
    int key=z;
    int *item = (int*)bsearch((void*)&key, (void*)&_atlasIndexArray->arr[0], _atlasIndexArray->num, sizeof(void*), compareInts);

    CCASSERT(item, "TMX atlas index not found. Shall not happen");

    ssize_t index = ((size_t)item - (size_t)_atlasIndexArray->arr) / sizeof(void*);
    return index;
}

ssize_t TMXLayer2::atlasIndexForNewZ(int z)
{
    // XXX: This can be improved with a sort of binary search
    ssize_t i=0;
    for (i=0; i< _atlasIndexArray->num ; i++) 
    {
        ssize_t val = (size_t) _atlasIndexArray->arr[i];
        if (z < val)
        {
            break;
        }
    } 
    
    return i;
}

// TMXLayer2 - adding / remove tiles
void TMXLayer2::setTileGID(int gid, const Point& pos)
{
    setTileGID(gid, pos, (ccTMXTileFlags)0);
}

void TMXLayer2::setTileGID(int gid, const Point& pos, ccTMXTileFlags flags)
{
}


//CCTMXLayer2 - obtaining positions, offset
Point TMXLayer2::calculateLayerOffset(const Point& pos)
{
    Point ret = Point::ZERO;
    switch (_layerOrientation) 
    {
    case TMXOrientationOrtho:
        ret = Point( pos.x * _mapTileSize.width, -pos.y *_mapTileSize.height);
        break;
    case TMXOrientationIso:
        ret = Point((_mapTileSize.width /2) * (pos.x - pos.y),
                  (_mapTileSize.height /2 ) * (-pos.x - pos.y));
        break;
    case TMXOrientationHex:
        CCASSERT(pos.equals(Point::ZERO), "offset for hexagonal map not implemented yet");
        break;
    }
    return ret;    
}

Point TMXLayer2::getPositionAt(const Point& pos)
{
    Point ret = Point::ZERO;
    switch (_layerOrientation)
    {
    case TMXOrientationOrtho:
        ret = getPositionForOrthoAt(pos);
        break;
    case TMXOrientationIso:
        ret = getPositionForIsoAt(pos);
        break;
    case TMXOrientationHex:
        ret = getPositionForHexAt(pos);
        break;
    }
    ret = CC_POINT_PIXELS_TO_POINTS( ret );
    return ret;
}

Point TMXLayer2::getPositionForOrthoAt(const Point& pos)
{
    return Point(pos.x * _mapTileSize.width,
                            (_layerSize.height - pos.y - 1) * _mapTileSize.height);
}

Point TMXLayer2::getPositionForIsoAt(const Point& pos)
{
    return Point(_mapTileSize.width /2 * (_layerSize.width + pos.x - pos.y - 1),
                             _mapTileSize.height /2 * ((_layerSize.height * 2 - pos.x - pos.y) - 2));
}

Point TMXLayer2::getPositionForHexAt(const Point& pos)
{
    float diffY = 0;
    if ((int)pos.x % 2 == 1)
    {
        diffY = -_mapTileSize.height/2 ;
    }

    Point xy = Point(pos.x * _mapTileSize.width*3/4,
                            (_layerSize.height - pos.y - 1) * _mapTileSize.height + diffY);
    return xy;
}

float TMXLayer2::getVertexZForPos(const Point& pos)
{
    float ret = 0;
    float maxVal = 0;
    if (_useAutomaticVertexZ)
    {
        switch (_layerOrientation) 
        {
        case TMXOrientationIso:
            maxVal = _layerSize.width + _layerSize.height;
            ret = -(maxVal - (pos.x + pos.y));
            break;
        case TMXOrientationOrtho:
            ret = -(_layerSize.height-pos.y);
            break;
        case TMXOrientationHex:
            CCASSERT(false, "TMX Hexa zOrder not supported");
            break;
        default:
            CCASSERT(false, "TMX invalid value");
            break;
        }
    } 
    else
    {
        ret = _vertexZvalue;
    }
    
    return ret;
}

std::string TMXLayer2::getDescription() const
{
    return StringUtils::format("<TMXLayer2 | tag = %d, size = %d,%d>", _tag, (int)_mapTileSize.width, (int)_mapTileSize.height);
}


NS_CC_END


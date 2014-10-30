 // Copyright NVIDIA Corporation 2002-2013
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#include "WRLLoader.h"
#include <dp/Exception.h>
#include <dp/sg/core/Billboard.h>
#include <dp/sg/core/GeoNode.h>
#include <dp/sg/core/IndexSet.h>
#include <dp/sg/core/LightSource.h>
#include <dp/sg/core/LOD.h>
#include <dp/sg/core/PerspectiveCamera.h>
#include <dp/sg/core/Sampler.h>
#include <dp/sg/core/Scene.h>
#include <dp/sg/core/TextureHost.h>
#include <dp/sg/core/Transform.h>
#include <dp/sg/core/Switch.h>
#include <dp/sg/io/IO.h>
#include <dp/sg/io/PlugInterfaceID.h>
#include <dp/util/File.h>
#include <dp/util/Tools.h>

#include <iterator>

using namespace dp::sg::core;
using namespace dp::math;
using namespace dp::util;
using namespace vrml;
using dp::util::PlugInCallback;
using dp::util::UPIID;
using dp::util::UPITID;
using dp::util::PlugIn;
using std::vector;
using std::string;
using std::map;
using std::pair;
using std::make_pair;
using std::multimap;

// Indices for the m_subdivisions array.
enum
{
  SUBDIVISION_SPHERE_MIN = 0,
  SUBDIVISION_SPHERE_DEFAULT,
  SUBDIVISION_SPHERE_MAX,
  SUBDIVISION_BOX_MIN,
  SUBDIVISION_BOX_DEFAULT,
  SUBDIVISION_BOX_MAX
};

const UPITID PITID_SCENE_LOADER(UPITID_SCENE_LOADER, UPITID_VERSION); // plug-in type
const UPIID  PIID_WRL_SCENE_LOADER(".WRL", PITID_SCENE_LOADER); // plug-in ID

#define readMFColor( mf )     readMFType<SFColor>( mf, &WRLLoader::readSFColor )
#define readMFFloat( mf )     readMFType<SFFloat>( mf, &WRLLoader::readSFFloat )
#define readMFInt32( mf )     readMFType<SFInt32>( mf, &WRLLoader::readSFInt32 )
#define readMFRotation( mf )  readMFType<SFRotation>( mf, &WRLLoader::readSFRotation )
#define readMFString( mf )    readMFType<SFString>( mf, &WRLLoader::readSFString )
#define readMFVec2f( mf )     readMFType<SFVec2f>( mf, &WRLLoader::readSFVec2f )
#define readMFVec3f( mf )     readMFType<SFVec3f>( mf, &WRLLoader::readSFVec3f )

#if defined(_WIN32)
BOOL APIENTRY DllMain(HANDLE hModule, DWORD reason, LPVOID lpReserved)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    int i=0;
  }

  return TRUE;
}
#elif defined(LINUX)
void lib_init()
{
  int i=0;
}
#endif

bool getPlugInterface(const UPIID& piid, dp::util::SmartPtr<dp::util::PlugIn> & pi)
{
  if ( piid==PIID_WRL_SCENE_LOADER )
  {
    pi = dp::util::SmartPtr<dp::util::PlugIn>( new WRLLoader() );
    return( !!pi );
  }
  return false;
}

void queryPlugInterfacePIIDs( std::vector<dp::util::UPIID> & piids )
{
  piids.clear();

  piids.push_back(PIID_WRL_SCENE_LOADER);
}

string::size_type findBraces( const string &str )
{
  string::size_type endIndex = str.length();
  for ( string::size_type i=0 ; i<endIndex ; i++ )
  {
    char c = str[i];
    if ( ( c == '{' ) || ( c == '}' ) || ( c == '[' ) || ( c == ']' ) )
    {
      return( i );
    }
  }
  return( string::npos );
}

string::size_type findDelimiter( const string &str, string::size_type startIndex )
{
  string::size_type endIndex = str.length();
  bool inString = false;
  for ( string::size_type i=startIndex ; i<endIndex ; i++ )
  {
    char c = str[i];
    if ( c == '"' )
    {
      inString = !inString;
    }
    else if ( !inString && ( ( c == ' ' ) || ( c == '\r' ) || ( c == '\t' ) || ( c == '\n' ) || ( c == ',' ) ) )
    {
      return( i );
    }
  }
  return( string::npos );
}

string::size_type findNotDelimiter( const string &str, string::size_type startIndex )
{
  string::size_type endIndex = str.length();
  for ( string::size_type i=startIndex ; i<endIndex ; i++ )
  {
    char c = str[i];
    if ( ( c != ' ' ) && ( c != '\r' ) && ( c != '\t' ) && ( c != '\n' ) && ( c != ',' ) )
    {
      return( i );
    }
  }
  return( string::npos );
}

SFInt32 max( const MFInt32 &mfInt32 )
{
  SFInt32 m = mfInt32[0];
  for ( size_t i=1 ; i<mfInt32.size() ; i++ )
  {
    if ( m < mfInt32[i] )
    {
      m = mfInt32[i];
    }
  }
  return( m );
}

SFInt32 min( const MFInt32 &mfInt32 )
{
  SFInt32 m = mfInt32[0];
  for ( size_t i=1 ; i<mfInt32.size() ; i++ )
  {
    if ( m > mfInt32[i] )
    {
      m = mfInt32[i];
    }
  }
  return( m );
}

WRLLoader::WRLLoader()
: m_eof(false)
, m_strict(false)
, m_nextTokenStart(string::npos)
, m_nextTokenEnd(string::npos)
, m_lineLength(4096)
, m_stepsPerUnit(60)
, m_fh(NULL)
, m_lineNumber(0)
, m_rootNode(nullptr)
, m_scene(nullptr)
, m_smoothTraverser(nullptr)
{
  m_line = (char *) malloc( m_lineLength + 1 );

  // The circular ones used for Cone, Cylinder, and Sphere.
  m_subdivisions[SUBDIVISION_SPHERE_MIN]     = 12;  // minimum       // 30 degrees
  m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] = 36;  // at radius 1.0 // 10 degrees
  m_subdivisions[SUBDIVISION_SPHERE_MAX]     = 90;  // maximum       //  4 degrees
  
  // The rectangular ones used for the Box. (It needs a much lower maximum!)
  m_subdivisions[SUBDIVISION_BOX_MIN]     = 2;  // minimum 
  m_subdivisions[SUBDIVISION_BOX_DEFAULT] = 4;  // at size 1.0! (Box default size is 2.0 though.)
  m_subdivisions[SUBDIVISION_BOX_MAX]     = 8;  // maximum

  // The user can define the subdivisions used for build built-in geometry!
  if ( const char * env = getenv( "NVSG_WRL_SUBDIVISIONS" ) )
  {
    std::string values( env );
    std::string token;
    string::size_type tokenEnd = 0;
    
    for (int i = 0; i < 6; ++i)
    {
      string::size_type tokenStart = findNotDelimiter( values, tokenEnd );
      if ( tokenStart != string::npos )
      {
        tokenEnd = findDelimiter( values, tokenStart );
        token.assign( values, tokenStart, tokenEnd - tokenStart );
        if ( !token.empty() )
        {
          m_subdivisions[i] = atoi( token.c_str() );
        }
      }
    }

    // Now make sure the input values are consistent.
    // Absolute minimum required for non-degenerated circular objects is 3.
    if ( m_subdivisions[SUBDIVISION_SPHERE_MIN] < 3 )
    {
      m_subdivisions[SUBDIVISION_SPHERE_MIN] = 3;
    }
    if ( m_subdivisions[SUBDIVISION_SPHERE_MAX] < m_subdivisions[SUBDIVISION_SPHERE_MIN] )
    {
      m_subdivisions[SUBDIVISION_SPHERE_MAX] = m_subdivisions[SUBDIVISION_SPHERE_MIN];
    }
    // Make sure the subdivision at radius 1.0 is within the limits.
    m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] = 
      clamp(m_subdivisions[SUBDIVISION_SPHERE_DEFAULT], m_subdivisions[SUBDIVISION_SPHERE_MIN], m_subdivisions[SUBDIVISION_SPHERE_MAX]);

    // Now the same for the Box:
    // Absolute minimum required for non-degenerated Box object is 2.
    if ( m_subdivisions[SUBDIVISION_BOX_MIN] < 2 )
    {
      m_subdivisions[SUBDIVISION_BOX_MIN] = 2;
    }
    if ( m_subdivisions[SUBDIVISION_BOX_MAX] < m_subdivisions[SUBDIVISION_BOX_MIN] )
    {
      m_subdivisions[SUBDIVISION_BOX_MAX] = m_subdivisions[SUBDIVISION_BOX_MIN];
    }
    // Make sure the subdivision at size 1.0 is within the limits.
    m_subdivisions[SUBDIVISION_BOX_DEFAULT] = 
      clamp(m_subdivisions[SUBDIVISION_BOX_DEFAULT], m_subdivisions[SUBDIVISION_BOX_MIN], m_subdivisions[SUBDIVISION_BOX_MAX]);
  }
}

WRLLoader::~WRLLoader()
{
  free( m_line );
}

void WRLLoader::createBox( SmartPtr<IndexedFaceSet>& pIndexedFaceSet, const SFVec3f& size, bool textured )
{
  float width  = size[0];
  float height = size[1];
  float depth  = size[2];

  // Tessellate with square quads when inside the unclamped range.
  int w = clamp( (int) (m_subdivisions[SUBDIVISION_BOX_DEFAULT] * width ), m_subdivisions[SUBDIVISION_BOX_MIN], m_subdivisions[SUBDIVISION_BOX_MAX] );
  int h = clamp( (int) (m_subdivisions[SUBDIVISION_BOX_DEFAULT] * height), m_subdivisions[SUBDIVISION_BOX_MIN], m_subdivisions[SUBDIVISION_BOX_MAX] );
  int d = clamp( (int) (m_subdivisions[SUBDIVISION_BOX_DEFAULT] * depth ), m_subdivisions[SUBDIVISION_BOX_MIN], m_subdivisions[SUBDIVISION_BOX_MAX] );

  size_t numVertices = 2 * (h * d + d * w + h * w);
  size_t numIndices  = 5 * ((h - 1) * (d - 1) + (d - 1) * (w - 1) + (h - 1) * (w - 1));
  
  Coordinate * pCoordinate = new Coordinate;
  pCoordinate->point.reserve( numVertices ); // vertices
  pIndexedFaceSet->coord = pCoordinate;
  pIndexedFaceSet->coordIndex.reserve( numIndices );

  Normal * pNormal = new Normal;
  pNormal->vector.reserve( numVertices ); // normals
  pIndexedFaceSet->normal = pNormal;
  pIndexedFaceSet->normalIndex.reserve( numIndices );
  pIndexedFaceSet->normalPerVertex = true; // Is the default.

  TextureCoordinate * pTextureCoordinate = nullptr;
  if ( textured )
  {
    pTextureCoordinate = new TextureCoordinate;
    pTextureCoordinate->point.reserve( numVertices ); // texcoords
    pIndexedFaceSet->texCoord = pTextureCoordinate;
    pIndexedFaceSet->texCoordIndex.reserve( numIndices );
  }

  float xCoord; 
  float yCoord;
  float zCoord;
  float uCoord;
  float vCoord;

  int indexOffset = 0; // The next sub-object will generate indices starting at this position.

  // Positive x-axis vertices, normals, texcoords:
  xCoord = width * 0.5f;
  for (int lat = 0; lat < h; lat++)
  {
    vCoord = (float) lat / (float) (h - 1); // [0.0, 1.0]
    yCoord = height * (vCoord - 0.5f);      // [-height/2, height/2]

    for (int lon = 0; lon < d; lon++)
    {
      uCoord = (float) lon / (float) (d - 1); // [0.0, 1.0]
      zCoord = -depth * (uCoord - 0.5f);      // [-depth/2, depth/2]
      
      pCoordinate->point.push_back( SFVec3f( xCoord, yCoord, zCoord ) );
      pNormal->vector.push_back( SFVec3f( 1.0f, 0.0f, 0.0f) );
      if (textured)
      {
        pTextureCoordinate->point.push_back( SFVec2f( uCoord, vCoord ) );
      }
    }
  }
  // Indices:
  for (int lat = 0; lat < h - 1; lat++)
  {
    for (int lon = 0; lon < d - 1; lon++)
    {
      int ll =  lat      * d +  lon     ;  // lower left
      int lr =  lat      * d + (lon + 1);  // lower right
      int ur = (lat + 1) * d + (lon + 1);  // upper right 
      int ul = (lat + 1) * d +  lon     ;  // upper left

      ll += indexOffset;
      lr += indexOffset;
      ur += indexOffset;
      ul += indexOffset;

      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ur );
      pIndexedFaceSet->coordIndex.push_back( ul );
      pIndexedFaceSet->coordIndex.push_back( -1 );
      
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ur );
      pIndexedFaceSet->normalIndex.push_back( ul );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ur );
        pIndexedFaceSet->texCoordIndex.push_back( ul );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  }
  indexOffset += h * d; 

  // Positive y-axis vertices, normals, texcoords:
  yCoord = height * 0.5f;
  for (int lat = 0; lat < d; lat++)
  {
    vCoord = (float) lat / (float) (d - 1); // [0.0, 1.0]
    zCoord = -depth * (vCoord - 0.5f);      // [-depth/2, depth/2]

    for (int lon = 0; lon < w; lon++)
    {
      uCoord = (float) lon / (float) (w - 1); // [0.0, 1.0]
      xCoord = width * (uCoord - 0.5f);       // [-width/2, width/2]
      
      pCoordinate->point.push_back( SFVec3f( xCoord, yCoord, zCoord ) );
      pNormal->vector.push_back( SFVec3f( 0.0f, 1.0f, 0.0f) );
      if (textured)
      {
        pTextureCoordinate->point.push_back( SFVec2f( uCoord, vCoord ) );
      }
    }
  }
  for (int lat = 0; lat < d - 1; lat++)
  {
    for (int lon = 0; lon < w - 1; lon++)
    {
      int ll =  lat      * w +  lon     ;  // lower left
      int lr =  lat      * w + (lon + 1);  // lower right
      int ur = (lat + 1) * w + (lon + 1);  // upper right 
      int ul = (lat + 1) * w +  lon     ;  // upper left

      ll += indexOffset;
      lr += indexOffset;
      ur += indexOffset;
      ul += indexOffset;

      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ur );
      pIndexedFaceSet->coordIndex.push_back( ul );
      pIndexedFaceSet->coordIndex.push_back( -1 );
      
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ur );
      pIndexedFaceSet->normalIndex.push_back( ul );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ur );
        pIndexedFaceSet->texCoordIndex.push_back( ul );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  }
  indexOffset += d * w; 

  // Positive z-axis vertices, normals, texcoords:
  zCoord = depth * 0.5f;
  for (int lat = 0; lat < h; lat++)
  {
    vCoord = (float) lat / (float) (h - 1); // [0.0, 1.0]
    yCoord = height * (vCoord - 0.5f);      // [-height/2, height/2]

    for (int lon = 0; lon < w; lon++)
    {
      uCoord = (float) lon / (float) (w - 1); // [0.0, 1.0]
      xCoord = width * (uCoord - 0.5f);       // [-width/2, width/2]
      
      pCoordinate->point.push_back( SFVec3f( xCoord, yCoord, zCoord ) );
      pNormal->vector.push_back( SFVec3f( 0.0f, 0.0f, 1.0f) );
      if (textured)
      {
        pTextureCoordinate->point.push_back( SFVec2f( uCoord, vCoord ) );
      }
    }
  }
  for (int lat = 0; lat < h - 1; lat++)
  {
    for (int lon = 0; lon < w - 1; lon++)
    {
      int ll =  lat      * w +  lon     ;  // lower left
      int lr =  lat      * w + (lon + 1);  // lower right
      int ur = (lat + 1) * w + (lon + 1);  // upper right 
      int ul = (lat + 1) * w +  lon     ;  // upper left

      ll += indexOffset;
      lr += indexOffset;
      ur += indexOffset;
      ul += indexOffset;

      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ur );
      pIndexedFaceSet->coordIndex.push_back( ul );
      pIndexedFaceSet->coordIndex.push_back( -1 );
      
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ur );
      pIndexedFaceSet->normalIndex.push_back( ul );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ur );
        pIndexedFaceSet->texCoordIndex.push_back( ul );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  }
  indexOffset += h * w;

  // Negative x-axis vertices, normals, texcoords:
  xCoord = -width * 0.5f;
  for (int lat = 0; lat < h; lat++)
  {
    vCoord = (float) lat / (float) (h - 1); // [0.0, 1.0]
    yCoord = height * (vCoord - 0.5f);      // [-height/2, height/2]

    for (int lon = 0; lon < d; lon++)
    {
      uCoord = (float) lon / (float) (d - 1); // [0.0, 1.0]
      zCoord = depth * (uCoord - 0.5f);       // [-depth/2, depth/2]
      
      pCoordinate->point.push_back( SFVec3f( xCoord, yCoord, zCoord ) );
      pNormal->vector.push_back( SFVec3f( -1.0f, 0.0f, 0.0f) );
      if (textured)
      {
        pTextureCoordinate->point.push_back( SFVec2f( uCoord, vCoord ) );
      }
    }
  }
  // Indices:
  for (int lat = 0; lat < h - 1; lat++)
  {
    for (int lon = 0; lon < d - 1; lon++)
    {
      int ll =  lat      * d +  lon     ;  // lower left
      int lr =  lat      * d + (lon + 1);  // lower right
      int ur = (lat + 1) * d + (lon + 1);  // upper right 
      int ul = (lat + 1) * d +  lon     ;  // upper left

      ll += indexOffset;
      lr += indexOffset;
      ur += indexOffset;
      ul += indexOffset;

      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ur );
      pIndexedFaceSet->coordIndex.push_back( ul );
      pIndexedFaceSet->coordIndex.push_back( -1 );
      
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ur );
      pIndexedFaceSet->normalIndex.push_back( ul );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ur );
        pIndexedFaceSet->texCoordIndex.push_back( ul );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  }
  indexOffset += h * d; 

  // Negative y-axis vertices, normals, texcoords:
  yCoord = -height * 0.5f;
  for (int lat = 0; lat < d; lat++)
  {
    vCoord = (float) lat / (float) (d - 1); // [0.0, 1.0]
    zCoord = depth * (vCoord - 0.5f);      // [-depth/2, depth/2]

    for (int lon = 0; lon < w; lon++)
    {
      uCoord = (float) lon / (float) (w - 1); // [0.0, 1.0]
      xCoord = width * (uCoord - 0.5f);       // [-width/2, width/2]
      
      pCoordinate->point.push_back( SFVec3f( xCoord, yCoord, zCoord ) );
      pNormal->vector.push_back( SFVec3f( 0.0f, -1.0f, 0.0f) );
      if (textured)
      {
        pTextureCoordinate->point.push_back( SFVec2f( uCoord, vCoord ) );
      }
    }
  }
  for (int lat = 0; lat < d - 1; lat++)
  {
    for (int lon = 0; lon < w - 1; lon++)
    {
      int ll =  lat      * w +  lon     ;  // lower left
      int lr =  lat      * w + (lon + 1);  // lower right
      int ur = (lat + 1) * w + (lon + 1);  // upper right 
      int ul = (lat + 1) * w +  lon     ;  // upper left

      ll += indexOffset;
      lr += indexOffset;
      ur += indexOffset;
      ul += indexOffset;

      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ur );
      pIndexedFaceSet->coordIndex.push_back( ul );
      pIndexedFaceSet->coordIndex.push_back( -1 );
      
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ur );
      pIndexedFaceSet->normalIndex.push_back( ul );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ur );
        pIndexedFaceSet->texCoordIndex.push_back( ul );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  }
  indexOffset += d * w; 

  // Negative z-axis vertices, normals, texcoords:
  zCoord = -depth * 0.5f;
  for (int lat = 0; lat < h; lat++)
  {
    vCoord = (float) lat / (float) (h - 1); // [0.0, 1.0]
    yCoord = height * (vCoord - 0.5f);      // [-height/2, height/2]

    for (int lon = 0; lon < w; lon++)
    {
      uCoord = (float) lon / (float) (w - 1); // [0.0, 1.0]
      xCoord = -width * (uCoord - 0.5f);      // [-width/2, width/2]
      
      pCoordinate->point.push_back( SFVec3f( xCoord, yCoord, zCoord ) );
      pNormal->vector.push_back( SFVec3f( 0.0f, 0.0f, -1.0f) );
      if (textured)
      {
        pTextureCoordinate->point.push_back( SFVec2f( uCoord, vCoord ) );
      }
    }
  }
  for (int lat = 0; lat < h - 1; lat++)
  {
    for (int lon = 0; lon < w - 1; lon++)
    {
      int ll =  lat      * w +  lon     ;  // lower left
      int lr =  lat      * w + (lon + 1);  // lower right
      int ur = (lat + 1) * w + (lon + 1);  // upper right 
      int ul = (lat + 1) * w +  lon     ;  // upper left

      ll += indexOffset;
      lr += indexOffset;
      ur += indexOffset;
      ul += indexOffset;

      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ur );
      pIndexedFaceSet->coordIndex.push_back( ul );
      pIndexedFaceSet->coordIndex.push_back( -1 );
      
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ur );
      pIndexedFaceSet->normalIndex.push_back( ul );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ur );
        pIndexedFaceSet->texCoordIndex.push_back( ul );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  }
  // indexOffset += h * w;
}

void WRLLoader::createCone( SmartPtr<IndexedFaceSet>& pIndexedFaceSet,
                            float radius, float height, 
                            bool bottom, bool side, bool textured )
{
  if ( !(bottom || side ) ) // Any geometry to create?
  {
    return;
  }

  // Tessellate with square quads when inside the unclamped range.
  int m = clamp( (int) (m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] * radius), m_subdivisions[SUBDIVISION_SPHERE_MIN], m_subdivisions[SUBDIVISION_SPHERE_MAX] );
  int n = clamp( (int) (m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] * height / (2.0f * PI)), 2, m_subdivisions[SUBDIVISION_SPHERE_MAX] );
  int k = clamp( (int) (m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] * radius / (2.0f * PI)), 2, m_subdivisions[SUBDIVISION_SPHERE_MAX] );

  size_t numVertices = 0;
  size_t numIndices  = 0;
  if (bottom)
  {
    numVertices += (k - 1) * m + 1;
    numIndices  += (k - 2) * m * 5 + m * 4;
  }
  if (side)
  {
    numVertices += n * (m + 1);
    numIndices  += (n - 1) * m * 5;
  }

  Coordinate * pCoordinate = new Coordinate;
  pCoordinate->point.reserve( numVertices ); // vertices
  pIndexedFaceSet->coord = pCoordinate;
  pIndexedFaceSet->coordIndex.reserve( numIndices );

  Normal * pNormal = new Normal;
  pNormal->vector.reserve( numVertices ); // normals
  pIndexedFaceSet->normal = pNormal;
  pIndexedFaceSet->normalIndex.reserve( numIndices );
  pIndexedFaceSet->normalPerVertex = true; // Is the default.

  TextureCoordinate * pTextureCoordinate = nullptr;
  if ( textured )
  {
    pTextureCoordinate = new TextureCoordinate;
    pTextureCoordinate->point.reserve( numVertices ); // texcoords
    pIndexedFaceSet->texCoord = pTextureCoordinate;
    pIndexedFaceSet->texCoordIndex.reserve( numIndices );
  }

  int indexOffset = 0; // The next sub-object will generate indices starting at this position.
  float phi_step = 2.0f * PI / (float) m;

  if (bottom)
  {
    float scaleDec = 1.0f / (float) (k - 1);
    float yCoord = -height * 0.5f;
    for (int lat = 0; lat < k - 1; lat++) // Exclude the pole.
    {
      float scale = 1.0f - (float) lat * scaleDec;
      for (int lon = 0; lon < m; lon++)
      {
        // VRML defines the texture coordinates to start at the back of the cone, 
        // which means all phi angles need to be offset by pi/2. Top and bottom tesselation must match.
        float phi = (float) lon * phi_step + PI_HALF;
        float sinPhi = sinf(phi);
        float cosPhi = cosf(phi);
        
        float xCoord =  cosPhi * scale;
        float zCoord = -sinPhi * scale;
        pCoordinate->point.push_back( SFVec3f( xCoord * radius, yCoord, zCoord * radius ) );
        pNormal->vector.push_back( SFVec3f( 0.0f, -1.0f, 0.0) ); // bottom
        if (textured)
        {
          // "The bottom texture appears right side up when the top of the cone is tilted toward the -Z axis"
          float texu =  zCoord * 0.5f + 0.5f; // [-1.0, 1.0] => [0.0, 1.0]
          float texv = -xCoord * 0.5f + 0.5f; 
          pTextureCoordinate->point.push_back( SFVec2f( texu, texv ) );
        }
      }
    }
  
    pCoordinate->point.push_back(SFVec3f(0.0f, yCoord, 0.0f));  // Center point (south pole).
    pNormal->vector.push_back(SFVec3f(0.0f, -1.0f, 0.0f));      // bottom
    if (textured)
    {
      pTextureCoordinate->point.push_back(SFVec2f(0.5f, 0.5f)); // texture center
    }

    for (int lat = 0; lat < k - 2; lat++)
    {                                           
      for (int lon = 0; lon < m; lon++)
      {
        int ll =  lat      * m + lon          ;  // lower left
        int lr =  lat      * m + (lon + 1) % m;  // lower right
        int ur = (lat + 1) * m + (lon + 1) % m;  // upper right 
        int ul = (lat + 1) * m + lon          ;  // upper left

        // Bottom disc inverts the winding!
        pIndexedFaceSet->coordIndex.push_back( ll );
        pIndexedFaceSet->coordIndex.push_back( ul );
        pIndexedFaceSet->coordIndex.push_back( ur );
        pIndexedFaceSet->coordIndex.push_back( lr );
        pIndexedFaceSet->coordIndex.push_back( -1 );

        pIndexedFaceSet->normalIndex.push_back( ll );
        pIndexedFaceSet->normalIndex.push_back( ul );
        pIndexedFaceSet->normalIndex.push_back( ur );
        pIndexedFaceSet->normalIndex.push_back( lr );
        pIndexedFaceSet->normalIndex.push_back( -1 );

        if (textured)
        {
          pIndexedFaceSet->texCoordIndex.push_back( ll );
          pIndexedFaceSet->texCoordIndex.push_back( ul );
          pIndexedFaceSet->texCoordIndex.push_back( ur );
          pIndexedFaceSet->texCoordIndex.push_back( lr );
          pIndexedFaceSet->texCoordIndex.push_back( -1 );
        }
      }
    }

    // Close the center.
    for (int lon = 0; lon < m; lon++)
    {
      int ll     = (k - 2) * m + lon;            // lower left
      int lr     = (k - 2) * m + (lon + 1) % m;  // lower right
      int center = (k - 1) * m;                  // center 

      // Bottom disc inverts the winding!
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( center );
      pIndexedFaceSet->coordIndex.push_back( -1 );

      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( center );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( center );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }

    indexOffset = (k - 1) * m + 1; // The next sub-object will generate indices starting at this position.
  } // bottom


  if (side)
  {
    Vec3f sideNormal(height, radius, 0.0f);
    normalize(sideNormal);

    // Latitudinal rings.
    // Starting at the bottom outer ring going upwards.
    for ( int lat = 0; lat < n; lat++ ) // Subdivisions along the height.
    {
      float texv = (float) lat / (float) (n - 1); // Range [0.0f, 1.0f]
      float yCoord = height * (texv - 0.5f);    // Range [-height/2, height/2]
      float scale = (1.0f - texv) * radius;

      // Generate vertices along the latitudinal rings.
      // On each latitude there are m + 1 vertices, 
      // the last one and the first one are on identical positions but have different texture coordinates.
      for ( int lon = 0; lon <= m; lon++ ) // phi angle
      {
        // VRML defines the texture coordinates to start at the back of the cone, 
        // which means all phi angles need to be offset by pi/2.
        float phi = (float) lon * phi_step + PI_HALF;
        float sinPhi = sinf( phi );
        float cosPhi = cosf( phi );
        float texu = (float) lon / (float) m; // Range [0.0f, 1.0f]
        
        pCoordinate->point.push_back( SFVec3f( cosPhi * scale, yCoord, -sinPhi * scale ) );
        // Rotate the side's normal around the y-axis.
        pNormal->vector.push_back( SFVec3f( cosPhi * sideNormal[0] + sinPhi * sideNormal[2], 
                                            sideNormal[1], 
                                           -sinPhi * sideNormal[0] + cosPhi * sideNormal[2] ) );
        if (textured)
        {
          pTextureCoordinate->point.push_back( SFVec2f( texu, texv ) );
        }
      }
    }
    
    // We have generated m + 1 vertices per lat.
    const int columns = m + 1;

    // Calculate indices. Using Quads for VRML.
    for ( int lat = 0; lat < n - 1; lat++ )
    {                                           
      for ( int lon = 0; lon < m; lon++ )
      {
        SFInt32 ll =  lat      * columns + lon    ;  // lower left
        SFInt32 lr =  lat      * columns + lon + 1;  // lower right
        SFInt32 ur = (lat + 1) * columns + lon + 1;  // upper right 
        SFInt32 ul = (lat + 1) * columns + lon    ;  // upper left

        ll += indexOffset;
        lr += indexOffset;
        ur += indexOffset;
        ul += indexOffset;

        pIndexedFaceSet->coordIndex.push_back( ll );
        pIndexedFaceSet->coordIndex.push_back( lr );
        pIndexedFaceSet->coordIndex.push_back( ur );
        pIndexedFaceSet->coordIndex.push_back( ul );
        pIndexedFaceSet->coordIndex.push_back( -1 );
        
        pIndexedFaceSet->normalIndex.push_back( ll );
        pIndexedFaceSet->normalIndex.push_back( lr );
        pIndexedFaceSet->normalIndex.push_back( ur );
        pIndexedFaceSet->normalIndex.push_back( ul );
        pIndexedFaceSet->normalIndex.push_back( -1 );

        if (textured)
        {
          pIndexedFaceSet->texCoordIndex.push_back( ll );
          pIndexedFaceSet->texCoordIndex.push_back( lr );
          pIndexedFaceSet->texCoordIndex.push_back( ur );
          pIndexedFaceSet->texCoordIndex.push_back( ul );
          pIndexedFaceSet->texCoordIndex.push_back( -1 );
        }
      }
    }
  } // side
}

void WRLLoader::createCylinder( SmartPtr<IndexedFaceSet>& pIndexedFaceSet,
                                float radius, float height, 
                                bool bottom, bool side, bool top, bool textured )
{
  if ( !(bottom || side || top) ) // Any geometry to create?
  {
    return; 
  }

  // Tessellate with square quads when inside the unclamped range.
  int m = clamp( (int) (m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] * radius), m_subdivisions[SUBDIVISION_SPHERE_MIN], m_subdivisions[SUBDIVISION_SPHERE_MAX] );
  int n = clamp( (int) (m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] * height / (2.0f * PI)), 2, m_subdivisions[SUBDIVISION_SPHERE_MAX] );
  int k = clamp( (int) (m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] * radius / (2.0f * PI)), 2, m_subdivisions[SUBDIVISION_SPHERE_MAX] );

  size_t numVertices = 0;
  size_t numIndices  = 0;
  if (bottom)
  {
    numVertices += (k - 1) * m + 1;
    numIndices  += (k - 2) * m * 5 + m * 4;
  }
  if (side)
  {
    numVertices += n * (m + 1);
    numIndices  += (n - 1) * m * 5;
  }
  if (top)
  {
    numVertices += (k - 1) * m + 1;
    numIndices  += (k - 2) * m * 5 + m * 4;
  }
  
  Coordinate * pCoordinate = new Coordinate;
  pCoordinate->point.reserve( numVertices ); // vertices
  pIndexedFaceSet->coord = pCoordinate;
  pIndexedFaceSet->coordIndex.reserve( numIndices );

  Normal * pNormal = new Normal;
  pNormal->vector.reserve( numVertices ); // normals
  pIndexedFaceSet->normal = pNormal;
  pIndexedFaceSet->normalIndex.reserve( numIndices );
  pIndexedFaceSet->normalPerVertex = true; // Is the default.

  TextureCoordinate * pTextureCoordinate = nullptr;
  if ( textured )
  {
    pTextureCoordinate = new TextureCoordinate;
    pTextureCoordinate->point.reserve( numVertices ); // texcoords
    pIndexedFaceSet->texCoord = pTextureCoordinate;
    pIndexedFaceSet->texCoordIndex.reserve( numIndices );
  }

  int indexOffset = 0; // The next sub-object will generate indices starting at this position.
  float phi_step = 2.0f * PI / (float) m;

  if (bottom)
  {
    float scaleDec = 1.0f / (float) (k - 1);
    float yCoord = -height * 0.5f;
    for (int lat = 0; lat < k - 1; lat++) // Exclude the pole.
    {
      float scale = 1.0f - (float) lat * scaleDec;
      for (int lon = 0; lon < m; lon++)
      {
        // VRML defines the texture coordinates to start at the back of the cylinder, 
        // which means all phi angles need to be offset by pi/2. Top and bottom tesselation must match!
        float phi = (float) lon * phi_step + PI_HALF;
        float sinPhi = sinf(phi);
        float cosPhi = cosf(phi);
        
        float xCoord =  cosPhi * scale;
        float zCoord = -sinPhi * scale;
        pCoordinate->point.push_back( SFVec3f( xCoord * radius, yCoord, zCoord * radius ) );
        pNormal->vector.push_back( SFVec3f( 0.0f, -1.0f, 0.0) ); // bottom
        if (textured)
        {
          // "The bottom texture appears right side up when the top of the cylinder is tilted toward the -Z axis"
          float texu =  zCoord * 0.5f + 0.5f; // [-1.0, 1.0] => [0.0, 1.0]
          float texv = -xCoord * 0.5f + 0.5f; 
          pTextureCoordinate->point.push_back( SFVec2f( texu, texv ) );
        }
      }
    }
  
    pCoordinate->point.push_back(SFVec3f(0.0f, yCoord, 0.0f));  // Center point (south pole).
    pNormal->vector.push_back(SFVec3f(0.0f, -1.0f, 0.0f));      // bottom
    if (textured)
    {
      pTextureCoordinate->point.push_back(SFVec2f(0.5f, 0.5f)); // texture center
    }

    for (int lat = 0; lat < k - 2; lat++)
    {                                           
      for (int lon = 0; lon < m; lon++)
      {
        int ll =  lat      * m + lon          ;  // lower left
        int lr =  lat      * m + (lon + 1) % m;  // lower right
        int ur = (lat + 1) * m + (lon + 1) % m;  // upper right 
        int ul = (lat + 1) * m + lon          ;  // upper left

        // Bottom disc inverts the winding!
        pIndexedFaceSet->coordIndex.push_back( ll );
        pIndexedFaceSet->coordIndex.push_back( ul );
        pIndexedFaceSet->coordIndex.push_back( ur );
        pIndexedFaceSet->coordIndex.push_back( lr );
        pIndexedFaceSet->coordIndex.push_back( -1 );

        pIndexedFaceSet->normalIndex.push_back( ll );
        pIndexedFaceSet->normalIndex.push_back( ul );
        pIndexedFaceSet->normalIndex.push_back( ur );
        pIndexedFaceSet->normalIndex.push_back( lr );
        pIndexedFaceSet->normalIndex.push_back( -1 );

        if (textured)
        {
          pIndexedFaceSet->texCoordIndex.push_back( ll );
          pIndexedFaceSet->texCoordIndex.push_back( ul );
          pIndexedFaceSet->texCoordIndex.push_back( ur );
          pIndexedFaceSet->texCoordIndex.push_back( lr );
          pIndexedFaceSet->texCoordIndex.push_back( -1 );
        }
      }
    }

    // Close the center.
    for (int lon = 0; lon < m; lon++)
    {
      int ll     = (k - 2) * m + lon;            // lower left
      int lr     = (k - 2) * m + (lon + 1) % m;  // lower right
      int center = (k - 1) * m;                  // center 

      // Bottom disc inverts the winding!
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( center );
      pIndexedFaceSet->coordIndex.push_back( -1 );

      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( center );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( center );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }

    indexOffset = (k - 1) * m + 1; // The next sub-object will generate indices starting at this position.
  } // bottom


  if (side)
  {
    // Latitudinal rings.
    // Starting at the bottom outer ring going upwards.
    for ( int lat = 0; lat < n; lat++ ) // Subdivisions along the height.
    {
      float texv = (float) lat / (float) (n - 1); // Range [0.0f, 1.0f]
      float yCoord = height * (texv - 0.5f);    // Range [-height/2, height/2]

      // Generate vertices along the latitudinal rings.
      // On each latitude there are m + 1 vertices, 
      // the last one and the first one are on identical positions but have different texture coordinates.
      for ( int lon = 0; lon <= m; lon++ ) // phi angle
      {
        // VRML defines the texture coordinates to start at the back of the cylinder, 
        // which means all phi angles need to be offset by pi/2.
        float phi = (float) lon * phi_step + PI_HALF;
        float sinPhi = sinf( phi );
        float cosPhi = cosf( phi );
        float texu = (float) lon / (float) m; // Range [0.0f, 1.0f]
        
        pCoordinate->point.push_back( SFVec3f( cosPhi * radius, yCoord, -sinPhi * radius ) );
        pNormal->vector.push_back( SFVec3f( cosPhi, 0.0f, -sinPhi ) );
        if (textured)
        {
          pTextureCoordinate->point.push_back( SFVec2f( texu, texv ) );
        }
      }
    }
    
    // We have generated m + 1 vertices per lat.
    const int columns = m + 1;

    // Calculate indices. Using Quads for VRML.
    for ( int lat = 0; lat < n - 1; lat++ )
    {                                           
      for ( int lon = 0; lon < m; lon++ )
      {
        SFInt32 ll =  lat      * columns + lon    ;  // lower left
        SFInt32 lr =  lat      * columns + lon + 1;  // lower right
        SFInt32 ur = (lat + 1) * columns + lon + 1;  // upper right 
        SFInt32 ul = (lat + 1) * columns + lon    ;  // upper left

        ll += indexOffset;
        lr += indexOffset;
        ur += indexOffset;
        ul += indexOffset;

        pIndexedFaceSet->coordIndex.push_back( ll );
        pIndexedFaceSet->coordIndex.push_back( lr );
        pIndexedFaceSet->coordIndex.push_back( ur );
        pIndexedFaceSet->coordIndex.push_back( ul );
        pIndexedFaceSet->coordIndex.push_back( -1 );
        
        pIndexedFaceSet->normalIndex.push_back( ll );
        pIndexedFaceSet->normalIndex.push_back( lr );
        pIndexedFaceSet->normalIndex.push_back( ur );
        pIndexedFaceSet->normalIndex.push_back( ul );
        pIndexedFaceSet->normalIndex.push_back( -1 );

        if (textured)
        {
          pIndexedFaceSet->texCoordIndex.push_back( ll );
          pIndexedFaceSet->texCoordIndex.push_back( lr );
          pIndexedFaceSet->texCoordIndex.push_back( ur );
          pIndexedFaceSet->texCoordIndex.push_back( ul );
          pIndexedFaceSet->texCoordIndex.push_back( -1 );
        }
      }
    }

    indexOffset += n * (m + 1); // This many vertices have been generated.
  } // side

  if (top)
  {
    float scaleDec = 1.0f / (float) (k - 1);
    float yCoord = height * 0.5f;
    for (int lat = 0; lat < k - 1; lat++) // Exclude the pole.
    {
      // Nicer, more regular shape of the triangles.
      float scale = 1.0f - (float) lat * scaleDec;
      for (int lon = 0; lon < m; lon++)
      {
        // VRML defines the texture coordinates to start at the back of the cylinder, 
        // which means all phi angles need to be offset by pi/2. Top and bottom tesselation must match.
        float phi = (float) lon * phi_step + PI_HALF;
        float sinPhi = sinf(phi);
        float cosPhi = cosf(phi);
        
        float xCoord =  cosPhi * scale; // [-1.0, 1.0]
        float zCoord = -sinPhi * scale;
        pCoordinate->point.push_back( SFVec3f( xCoord * radius, yCoord, zCoord * radius ) );
        pNormal->vector.push_back( SFVec3f( 0.0f, 1.0f, 0.0) ); // top
        if (textured)
        {
          // "The top texture appears right side up when the top of the cylinder is tilted toward the +Z axis"
          float texu = -zCoord * 0.5f + 0.5f; // [-1.0, 1.0] => [0.0, 1.0]
          float texv = -xCoord * 0.5f + 0.5f; 
          pTextureCoordinate->point.push_back( SFVec2f( texu, texv ) );
        }
      }
    }
  
    pCoordinate->point.push_back(SFVec3f(0.0f, yCoord, 0.0f));  // Center point (north pole).
    pNormal->vector.push_back(SFVec3f(0.0f, 1.0f, 0.0f));       // top
    if (textured)
    {
      pTextureCoordinate->point.push_back(SFVec2f(0.5f, 0.5f)); // texture center
    }

    for (int lat = 0; lat < k - 2; lat++)
    {                                           
      for (int lon = 0; lon < m; lon++)
      {
        int ll =  lat      * m + lon          ;  // lower left
        int lr =  lat      * m + (lon + 1) % m;  // lower right
        int ur = (lat + 1) * m + (lon + 1) % m;  // upper right 
        int ul = (lat + 1) * m + lon          ;  // upper left

        ll += indexOffset;
        lr += indexOffset;
        ur += indexOffset;
        ul += indexOffset;
        
        // Top disc uses standard CCW ordering.
        pIndexedFaceSet->coordIndex.push_back( ll );
        pIndexedFaceSet->coordIndex.push_back( lr );
        pIndexedFaceSet->coordIndex.push_back( ur );
        pIndexedFaceSet->coordIndex.push_back( ul );
        pIndexedFaceSet->coordIndex.push_back( -1 );
        
        pIndexedFaceSet->normalIndex.push_back( ll );
        pIndexedFaceSet->normalIndex.push_back( lr );
        pIndexedFaceSet->normalIndex.push_back( ur );
        pIndexedFaceSet->normalIndex.push_back( ul );
        pIndexedFaceSet->normalIndex.push_back( -1 );

        if (textured)
        {
          pIndexedFaceSet->texCoordIndex.push_back( ll );
          pIndexedFaceSet->texCoordIndex.push_back( lr );
          pIndexedFaceSet->texCoordIndex.push_back( ur );
          pIndexedFaceSet->texCoordIndex.push_back( ul );
          pIndexedFaceSet->texCoordIndex.push_back( -1 );
        }
      }
    }

    // Close the center.
    for (int lon = 0; lon < m; lon++)
    {
      int ll     = (k - 2) * m + lon;            // lower left
      int lr     = (k - 2) * m + (lon + 1) % m;  // lower right
      int center = (k - 1) * m;                  // center 

      ll     += indexOffset;
      lr     += indexOffset;
      center += indexOffset;
      
      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( center );
      pIndexedFaceSet->coordIndex.push_back( -1 );

      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( center );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( center );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  } // top
}

void WRLLoader::createSphere( SmartPtr<IndexedFaceSet>& pIndexedFaceSet, float radius, bool textured )
{
  int m = clamp( (int) (m_subdivisions[SUBDIVISION_SPHERE_DEFAULT] * radius), m_subdivisions[SUBDIVISION_SPHERE_MIN], m_subdivisions[SUBDIVISION_SPHERE_MAX] );
  int n = clamp( m >> 1, std::max(3, m_subdivisions[SUBDIVISION_SPHERE_MIN] >> 1), std::max(3, m_subdivisions[SUBDIVISION_SPHERE_MAX] >> 1) );

  const size_t numVertices = n * (m + 1);     // Number of vertices.
  const size_t numIndices  = (n - 1) * m * 5; // Number of indices (quad plus -1 end index = 5)

  Coordinate * pCoordinate = new Coordinate;
  pCoordinate->point.reserve( numVertices ); // vertices
  pIndexedFaceSet->coord = pCoordinate;
  pIndexedFaceSet->coordIndex.reserve( numIndices );

  Normal * pNormal = new Normal;
  pNormal->vector.reserve( numVertices ); // normals
  pIndexedFaceSet->normal = pNormal;
  pIndexedFaceSet->normalIndex.reserve( numIndices );
  pIndexedFaceSet->normalPerVertex = true; // Is the default.

  TextureCoordinate * pTextureCoordinate = nullptr;
  if ( textured )
  {
    pTextureCoordinate = new TextureCoordinate;
    pTextureCoordinate->point.reserve( numVertices ); // texcoords
    pIndexedFaceSet->texCoord = pTextureCoordinate;
    pIndexedFaceSet->texCoordIndex.reserve( numIndices );
  }

  float phi_step = 2.0f * PI / (float) m;
  float theta_step = PI / (float) (n - 1);

  // Latitudinal rings.
  // Starting at the south pole going upwards.
  for ( int latitude = 0; latitude < n; latitude++ ) // theta angle
  {
    float theta = (float) latitude * theta_step;
    float sinTheta = sinf( theta );
    float cosTheta = cosf( theta );
    float texv = (float) latitude / (float) (n - 1); // Range [0.0f, 1.0f]

    // Generate vertices along the latitudinal rings.
    // On each latitude there are m + 1 vertices, 
    // the last one and the first one are on identical positions but have different texture coordinates.
    for ( int longitude = 0; longitude <= m; longitude++ ) // phi angle
    {
      // VRML defines the texture coordinates to start at the back of the sphere, 
      // which means all phi angles need to be offset by pi/2.
      float phi = (float) longitude * phi_step + PI_HALF;
      float sinPhi = sinf( phi );
      float cosPhi = cosf( phi );
      float texu = (float) longitude / (float) m; // Range [0.0f, 1.0f]
      
      // Unit sphere coordinates are the normals.
      SFVec3f v = SFVec3f( cosPhi * sinTheta, 
                          -cosTheta,             // -y to start at the south pole.
                          -sinPhi * sinTheta );

      pCoordinate->point.push_back( v * radius );
      pNormal->vector.push_back( v );
      if (textured)
      {
        pTextureCoordinate->point.push_back( SFVec2f( texu, texv ) );
      }
    }
  }
  
  // We have generated m + 1 vertices per latitude.
  const int columns = m + 1;

  // Calculate indices. Using Quads for VRML.
  for ( int latitude = 0; latitude < n - 1; latitude++ )
  {                                           
    for ( int longitude = 0; longitude < m; longitude++ )
    {
      SFInt32 ll =  latitude      * columns + longitude    ;  // lower left
      SFInt32 lr =  latitude      * columns + longitude + 1;  // lower right
      SFInt32 ur = (latitude + 1) * columns + longitude + 1;  // upper right 
      SFInt32 ul = (latitude + 1) * columns + longitude    ;  // upper left

      pIndexedFaceSet->coordIndex.push_back( ll );
      pIndexedFaceSet->coordIndex.push_back( lr );
      pIndexedFaceSet->coordIndex.push_back( ur );
      pIndexedFaceSet->coordIndex.push_back( ul );
      pIndexedFaceSet->coordIndex.push_back( -1 );
      
      pIndexedFaceSet->normalIndex.push_back( ll );
      pIndexedFaceSet->normalIndex.push_back( lr );
      pIndexedFaceSet->normalIndex.push_back( ur );
      pIndexedFaceSet->normalIndex.push_back( ul );
      pIndexedFaceSet->normalIndex.push_back( -1 );

      if (textured)
      {
        pIndexedFaceSet->texCoordIndex.push_back( ll );
        pIndexedFaceSet->texCoordIndex.push_back( lr );
        pIndexedFaceSet->texCoordIndex.push_back( ur );
        pIndexedFaceSet->texCoordIndex.push_back( ul );
        pIndexedFaceSet->texCoordIndex.push_back( -1 );
      }
    }
  }
}

void WRLLoader::determineTexGen( const SmartPtr<IndexedFaceSet> & pIndexedFaceSet
                               , const ParameterGroupDataSharedPtr & parameterGroupData )
{
  DP_ASSERT( pIndexedFaceSet && parameterGroupData );
  DP_ASSERT( isSmartPtrOf<Coordinate>(pIndexedFaceSet->coord) );

  const MFVec3f & point = smart_cast<Coordinate>(pIndexedFaceSet->coord)->point;
  SFVec3f min = point[0];
  SFVec3f max = min;
  for ( size_t i=1 ; i<point.size() ; i++ )
  {
    for ( unsigned int j=0 ; j<3 ; j++ )
    {
      if ( max[j] < point[i][j] )
      {
        max[j] = point[i][j];
      }
      else if ( point[i][j] < min[j] )
      {
        min[j] = point[i][j];
      }
    }
  }
  SFVec3f dim = max - min;
  unsigned int first, second;
  if ( dim[0] < dim[1] )
  {
    if ( dim[1] < dim[2] )
    {
      first = 2;
      second = 1;
    }
    else
    {
      first = 1;
      second = ( dim[0] < dim[2] ) ? 2 : 0;
    }
  }
  else
  {
    if ( dim[0] < dim[2] )
    {
      first = 2;
      second = 0;
    }
    else
    {
      first = 0;
      second = ( dim[1] < dim[2] ) ? 2 : 1;
    }
  }
  Vec4f plane[2];

  Plane3f p0( Vec3f( (first==0)?1.0f:0.0f, (first==1)?1.0f:0.0f, 
                     (first==2)?1.0f:0.0f ), min );
  plane[0] = Vec4f( p0.getNormal(), p0.getOffset() );

  Plane3f p1( Vec3f( (second==0)?1.0f:0.0f, (second==1)?1.0f:0.0f, 
                     (second==2)?1.0f:0.0f ), min );
  plane[1] = Vec4f( p0.getNormal(), p0.getOffset() );

  const dp::fx::SmartParameterGroupSpec & pgs = parameterGroupData->getParameterGroupSpec();
  DP_ASSERT( pgs->getName() == "standardTextureParameters" );
  DP_VERIFY( parameterGroupData->setParameterArrayElement<dp::fx::EnumSpec::StorageType>( "genMode", TCA_S, TGM_OBJECT_LINEAR ) );
  DP_VERIFY( parameterGroupData->setParameterArrayElement<dp::fx::EnumSpec::StorageType>( "genMode", TCA_T, TGM_OBJECT_LINEAR ) );
  DP_VERIFY( parameterGroupData->setParameterArrayElement<Vec4f>( "texGenPlane", TCA_S, plane[0] ) );
  DP_VERIFY( parameterGroupData->setParameterArrayElement<Vec4f>( "texGenPlane", TCA_T, plane[1] ) );
}

template<typename VType>
void  WRLLoader::eraseIndex( unsigned int f, unsigned int i, unsigned int count, bool perVertex
                           , MFInt32 &index, VType &vec )
{
  if ( perVertex )
  {
    if ( index.empty() )
    {
      //  do nothing, the indices are already erased from the vertex index array
    }
    else
    {
      //  remove count indices
      index.erase( index.begin()+i, index.begin()+i+count );
    }
  }
  else
  {
    if ( index.empty() )
    {
      //  remove the entry from the vector itself
      vec.erase( vec.begin()+f, vec.begin()+f+1 );
    }
    else
    {
      //  remove the single entry from the index
      DP_ASSERT( f < index.size() );
      index.erase( index.begin()+f, index.begin()+f+1 );
    }
  }
}

SFNode  WRLLoader::findNode( const SFNode currentNode, string name )
{
  SFNode  node = NULL;
  if ( m_defNodes.find( name ) != m_defNodes.end() )
  {
    node = m_defNodes[name];
  }
  else if ( currentNode && ( currentNode->getName() == name ) )
  {
    node = currentNode;
  }
  else
  {
    for ( int i=(int)m_openNodes.size()-1 ; i>0 ; i-- )
    {
      if ( m_openNodes[i]->getName() == name )
      {
        node = m_openNodes[i];
        i = -1;
      }
    }
  }
  return( node );
}

vector<unsigned int> WRLLoader::getCombinedKeys( const SmartPtr<PositionInterpolator> & center
                                               , const SmartPtr<OrientationInterpolator> & rotation
                                               , const SmartPtr<PositionInterpolator> & scale
                                               , const SmartPtr<PositionInterpolator> & translation )
{
  vector<unsigned int> steps[4];
  unsigned int n = 0;
  if ( center )
  {
    DP_ASSERT( center->interpreted );
    steps[n++] = center->steps;
  }
  if ( rotation )
  {
    DP_ASSERT( rotation->interpreted );
    steps[n++] = rotation->steps;
  }
  if ( scale )
  {
    DP_ASSERT( scale->interpreted );
    steps[n++] = scale->steps;
  }
  if ( translation )
  {
    DP_ASSERT( translation->interpreted );
    steps[n++] = translation->steps;
  }
  DP_ASSERT( n > 0 );
  vector<unsigned int> combinedSteps = steps[0];

  bool ok = true;
  for ( unsigned int i=1 ; ok && i<n ; i++ )
  {
    ok = ( steps[i-1].size() == steps[i].size() );
    for ( size_t j=0 ; ok && j<steps[i].size() ; j++ )
    {
      ok = ( steps[i-1][j] == steps[i][j] );
    }
  }
  if ( ! ok )
  {
    for ( unsigned int i=1 ; i<n ; i++ )
    {
      vector<unsigned int> tmpSteps;
      merge( combinedSteps.begin(), combinedSteps.end(), steps[i].begin(), steps[i].end()
           , back_inserter( tmpSteps ) );
      combinedSteps.clear();
      unique_copy( tmpSteps.begin(), tmpSteps.end(), back_inserter( combinedSteps ) );
    }
  }

  return( combinedSteps );
}

bool WRLLoader::getNextLine( void )
{
  string::size_type index;
  do
  {
    m_eof = ( fgets( m_line, m_lineLength+1, m_fh ) == NULL );
    while ( !m_eof && ( strlen( m_line ) == m_lineLength ) && ( m_line[m_lineLength-1] != '\n' ) )
    {
      m_line = (char *) realloc( m_line, 2 * m_lineLength + 1 );
      m_eof = ( fgets( &m_line[m_lineLength], m_lineLength+1, m_fh ) == NULL );
      m_lineLength *= 2;
    }
    if ( !m_eof )
    {
      DP_ASSERT( strlen( m_line ) <= m_lineLength );
      m_currentString = m_line;
      index = findNotDelimiter( m_currentString, 0 );   // find_first_not_of is slower!
      m_lineNumber++;
    }
  } while ( !m_eof && ( index == string::npos ) );
  return( !m_eof );
}

string & WRLLoader::getNextToken( void )
{
  if ( ! m_ungetToken.empty() )
  {
    m_currentToken = m_ungetToken;
    m_ungetToken.clear();
  }
  else if ( m_eof )
  {
    m_currentToken.clear();
  }
  else
  {
    DP_ASSERT( m_nextTokenStart < m_nextTokenEnd );
    DP_ASSERT( ( m_nextTokenEnd == string::npos ) || ( m_nextTokenEnd < m_currentString.length() ) );
    m_currentToken.assign( m_currentString, m_nextTokenStart, m_nextTokenEnd-m_nextTokenStart );
    DP_ASSERT( m_currentToken[0] != '#' );
    setNextToken();
  }

  if ( m_currentToken.length() > 1 )
  {
    string::size_type index = findBraces( m_currentToken );   // find_first_of is slower!
    if ( index != string::npos )
    {
      if ( index == 0 )
      {
        m_ungetToken.assign( m_currentToken, 1, string::npos );
        m_currentToken.erase( 1, string::npos );
      }
      else
      {
        m_ungetToken.assign( m_currentToken, index, string::npos );
        m_currentToken.erase( index, string::npos );
      }
    }
  }
  return( m_currentToken );
}

SFNode  WRLLoader::getNode( const string &nodeName, string &token )
{
  SFNode  n;
  if ( token == "Anchor" )
  {
    n = smart_cast<vrml::Object>(readAnchor( nodeName ));
  }
  else if ( token == "Appearance" )
  {
    n = smart_cast<vrml::Object>(readAppearance( nodeName ));
  }
  else if ( token == "AudioClip" )
  {
    n = smart_cast<vrml::Object>(readAudioClip( nodeName ));
  }
  else if ( token == "Background" )
  {
    n = smart_cast<vrml::Object>(readBackground( nodeName ));
  }
  else if ( token == "Billboard" )
  {
    n = smart_cast<vrml::Object>(readBillboard( nodeName ));
  }
  else if ( token == "Box" )
  {
    n = smart_cast<vrml::Object>(readBox( nodeName ));
  }
  else if ( token == "Collision" )
  {
    n = smart_cast<vrml::Object>(readCollision( nodeName ));
  }
  else if ( token == "Color" )
  {
    n = smart_cast<vrml::Object>(readColor( nodeName ));
  }
  else if ( token == "ColorInterpolator" )
  {
    n = smart_cast<vrml::Object>(readColorInterpolator( nodeName ));
  }
  else if ( token == "Cone" )
  {
    n = smart_cast<vrml::Object>(readCone( nodeName ));
  }
  else if ( token == "Coordinate" )
  {
    n = smart_cast<vrml::Object>(readCoordinate( nodeName ));
  }
  else if ( token == "CoordinateInterpolator" )
  {
    n = smart_cast<vrml::Object>(readCoordinateInterpolator( nodeName ));
  }
  else if ( token == "Cylinder" )
  {
    n = smart_cast<vrml::Object>(readCylinder( nodeName ));
  }
  else if ( token == "CylinderSensor" )
  {
    n = smart_cast<vrml::Object>(readCylinderSensor( nodeName ));
  }
  else if ( token == "DirectionalLight" )
  {
    n = smart_cast<vrml::Object>(readDirectionalLight( nodeName ));
  }
  else if ( token == "ElevationGrid" )
  {
    n = smart_cast<vrml::Object>(readElevationGrid( nodeName ));
  }
  else if ( token == "Extrusion" )
  {
    n = smart_cast<vrml::Object>(readExtrusion( nodeName ));
  }
  else if ( token == "Fog" )
  {
    n = smart_cast<vrml::Object>(readFog( nodeName ));
  }
  else if ( token == "FontStyle" )
  {
    n = smart_cast<vrml::Object>(readFontStyle( nodeName ));
  }
  else if ( token == "Group" )
  {
    n = smart_cast<vrml::Object>(readGroup( nodeName ));
  }
  else if ( token == "ImageTexture" )
  {
    n = smart_cast<vrml::Object>(readImageTexture( nodeName ));
  }
  else if ( token == "IndexedFaceSet" )
  {
    n = smart_cast<vrml::Object>(readIndexedFaceSet( nodeName ));
  }
  else if ( token == "IndexedLineSet" )
  {
    n = smart_cast<vrml::Object>(readIndexedLineSet( nodeName ));
  }
  else if ( token == "Inline" )
  {
    n = smart_cast<vrml::Object>(readInline( nodeName ));
  }
  else if ( token == "LOD" )
  {
    n = smart_cast<vrml::Object>(readLOD( nodeName ));
  }
  else if ( token == "Material" )
  {
    n = smart_cast<vrml::Object>(readMaterial( nodeName ));
  }
  else if ( token == "MovieTexture" )
  {
    n = smart_cast<vrml::Object>(readMovieTexture( nodeName ));
  }
  else if ( token == "NavigationInfo" )
  {
    n = smart_cast<vrml::Object>(readNavigationInfo( nodeName ));
  }
  else if ( token == "Normal" )
  {
    n = smart_cast<vrml::Object>(readNormal( nodeName ));
  }
  else if ( token == "NormalInterpolator" )
  {
    n = smart_cast<vrml::Object>(readNormalInterpolator( nodeName ));
  }
  else if ( token == "OrientationInterpolator" )
  {
    n = smart_cast<vrml::Object>(readOrientationInterpolator( nodeName ));
  }
  else if ( token == "PixelTexture" )
  {
    n = smart_cast<vrml::Object>(readPixelTexture( nodeName ));
  }
  else if ( token == "PlaneSensor" )
  {
    n = smart_cast<vrml::Object>(readPlaneSensor( nodeName ));
  }
  else if ( token == "PointLight" )
  {
    n = smart_cast<vrml::Object>(readPointLight( nodeName ));
  }
  else if ( token == "PointSet" )
  {
    n = smart_cast<vrml::Object>(readPointSet( nodeName ));
  }
  else if ( token == "PositionInterpolator" )
  {
    n = smart_cast<vrml::Object>(readPositionInterpolator( nodeName ));
  }
  else if ( token == "ProximitySensor" )
  {
    n = smart_cast<vrml::Object>(readProximitySensor( nodeName ));
  }
  else if ( token == "ScalarInterpolator" )
  {
    n = smart_cast<vrml::Object>(readScalarInterpolator( nodeName ));
  }
  else if ( token == "Script" )
  {
    n = smart_cast<vrml::Object>(readScript( nodeName ));
  }
  else if ( token == "Shape" )
  {
    n = smart_cast<vrml::Object>(readShape( nodeName ));
  }
  else if ( token == "Sound" )
  {
    n = smart_cast<vrml::Object>(readSound( nodeName ));
  }
  else if ( token == "Sphere" )
  {
    n = smart_cast<vrml::Object>(readSphere( nodeName ));
  }
  else if ( token == "SphereSensor" )
  {
    n = smart_cast<vrml::Object>(readSphereSensor( nodeName ));
  }
  else if ( token == "SpotLight" )
  {
    n = smart_cast<vrml::Object>(readSpotLight( nodeName ));
  }
  else if ( token == "Switch" )
  {
    n = smart_cast<vrml::Object>(readSwitch( nodeName ));
  }
  else if ( token == "Text" )
  {
    n = smart_cast<vrml::Object>(readText( nodeName ));
  }
  else if ( token == "TextureCoordinate" )
  {
    n = smart_cast<vrml::Object>(readTextureCoordinate( nodeName ));
  }
  else if ( token == "TextureTransform" )
  {
    n = smart_cast<vrml::Object>(readTextureTransform( nodeName ));
  }
  else if ( token == "TimeSensor" )
  {
    n = smart_cast<vrml::Object>(readTimeSensor( nodeName ));
  }
  else if ( token == "TouchSensor" )
  {
    n = smart_cast<vrml::Object>(readTouchSensor( nodeName ));
  }
  else if ( token == "Transform" )
  {
    n = smart_cast<vrml::Object>(readTransform( nodeName ));
  }
  else if ( token == "Viewpoint" )
  {
    n = smart_cast<vrml::Object>(readViewpoint( nodeName ));
  }
  else if ( token == "VisibilitySensor" )
  {
    n = smart_cast<vrml::Object>(readVisibilitySensor( nodeName ));
  }
  else if ( token == "WorldInfo" )
  {
    n = smart_cast<vrml::Object>(readWorldInfo( nodeName ));
  }
  else if ( m_PROTONames.find( token ) != m_PROTONames.end() )
  {
    onUnsupportedToken( "PROTO", token );
    ignoreBlock( "{", "}", getNextToken() );
    n = NULL;
  }
  else
  {
    onUnknownToken( "Node Type", token );
    n = NULL;
  }

  return( n );
}

SceneSharedPtr WRLLoader::import( const string &filename )
{
  m_fh = fopen( filename.c_str(), "r" );
  if ( m_fh )
  {
    if ( testWRLVersion( filename ) )
    {
      m_topLevelGroup = new vrml::Group;
      readStatements();
      interpretVRMLTree();
      m_topLevelGroup.reset();

      DP_ASSERT( m_scene && m_scene->getRootNode());
      //  clear the defNodes and inlines now
      m_defNodes.clear();
      m_inlines.clear();

      //  clean up the scene a bit: remove empty Triangles, shift ligths,...

      //  WRL files don't have target distances, so calculate them here...
      if ( m_scene->getNumberOfCameras() )
      {
        Sphere3f bs = m_scene->getBoundingSphere();
        if ( isPositive( bs ) )
        {
          for ( Scene::CameraIterator scci = m_scene->beginCameras() ; scci != m_scene->endCameras() ; ++scci )
          {
            DP_ASSERT( scci->isPtrTo<PerspectiveCamera>() );
            PerspectiveCameraSharedPtr const& pc = scci->staticCast<PerspectiveCamera>();
            pc->calcNearFarDistances( bs );
            pc->setFocusDistance( 0.5f * ( pc->getNearDistance() + pc->getFarDistance() ) );
          }
        }
      }
    }
    fclose( m_fh );
    m_fh = NULL;
  }

  return( m_scene );
}

void  WRLLoader::interpretVRMLTree( void )
{
  m_scene = Scene::create();    //  This is may be used while interpreting children
  m_rootNode = dp::sg::core::Group::create();
  {
    interpretChildren( m_topLevelGroup->children, m_rootNode );
  }
  
  m_scene->setRootNode( m_rootNode );
}

EffectDataSharedPtr WRLLoader::interpretAppearance( const SmartPtr<Appearance> & pAppearance )
{
  DP_ASSERT( pAppearance->material || pAppearance->texture );

  if ( ! pAppearance->materialEffect )
  {
    pAppearance->materialEffect = dp::sg::core::EffectData::create( getStandardMaterialSpec() );
    pAppearance->materialEffect->setName( pAppearance->getName() );

    if ( pAppearance->material )
    { 
      DP_ASSERT( isSmartPtrOf<vrml::Material>(pAppearance->material) );
      DP_VERIFY( pAppearance->materialEffect->setParameterGroupData( interpretMaterial( smart_cast<vrml::Material>(pAppearance->material) ) ) );
    }

    ParameterGroupDataSharedPtr textureData;
    if ( pAppearance->texture )
    {
      DP_ASSERT( isSmartPtrOf<vrml::Texture>(pAppearance->texture) );
      textureData = interpretTexture( smart_cast<vrml::Texture>(pAppearance->texture) );

      if ( textureData && pAppearance->textureTransform )
      {
        DP_ASSERT( isSmartPtrOf<TextureTransform>(pAppearance->textureTransform) );
        interpretTextureTransform( smart_cast<TextureTransform>(pAppearance->textureTransform), textureData );
      }

      DP_VERIFY( pAppearance->materialEffect->setParameterGroupData( textureData ) );
    }

    bool transparent = ( pAppearance->material && ( 0.0f < smart_cast<vrml::Material>(pAppearance->material)->transparency ) );
    if ( textureData && ! transparent )
    {
      const dp::fx::SmartParameterGroupSpec & pgs = textureData->getParameterGroupSpec();
      const SamplerSharedPtr & sampler = textureData->getParameter<SamplerSharedPtr>( pgs->findParameterSpec( "sampler" ) );
      if ( sampler )
      {
        const TextureSharedPtr & texture = sampler->getTexture();
        if ( texture && texture.isPtrTo<TextureHost>() )
        {
          Image::PixelFormat ipf = texture.staticCast<TextureHost>()->getFormat();
          transparent =   ( ipf == Image::IMG_RGBA )
                      ||  ( ipf == Image::IMG_BGRA )
                      ||  ( ipf == Image::IMG_LUMINANCE_ALPHA );
        }
      }
    }
    pAppearance->materialEffect->setTransparent( transparent );
  }

  return( pAppearance->materialEffect );
}

void  WRLLoader::interpretBackground( const SmartPtr<Background> & pBackground )
{
  //  just set the background color
  m_scene->setBackColor( Vec4f(interpretSFColor( pBackground->skyColor[0] ) ,1.0f));
}

BillboardSharedPtr WRLLoader::interpretBillboard( const SmartPtr<vrml::Billboard> & pVRMLBillboard )
{
  BillboardSharedPtr pNVSGBillboard;

  if ( pVRMLBillboard->pBillboard )
  {
    pNVSGBillboard = pVRMLBillboard->pBillboard;
  }
  else
  {
    pNVSGBillboard = dp::sg::core::Billboard::create();

    pNVSGBillboard->setName( pVRMLBillboard->getName() );
    if ( length( pVRMLBillboard->axisOfRotation ) < FLT_EPSILON )
    {
      pNVSGBillboard->setAlignment( dp::sg::core::Billboard::BA_VIEWER );
    }
    else
    {
      pNVSGBillboard->setAlignment( dp::sg::core::Billboard::BA_AXIS );
      pVRMLBillboard->axisOfRotation.normalize();
      pNVSGBillboard->setRotationAxis( pVRMLBillboard->axisOfRotation );
    }
    interpretChildren( pVRMLBillboard->children, pNVSGBillboard );
    pVRMLBillboard->pBillboard = pNVSGBillboard;
  }

  return( pNVSGBillboard );
}

inline bool evalTextured( const dp::sg::core::PrimitiveSharedPtr & pset, bool textured)
{
  if ( pset )
  {
    bool hasTexCoords = false;
    for ( unsigned int i=VertexAttributeSet::NVSG_TEXCOORD0
        ; !hasTexCoords && i<VertexAttributeSet::NVSG_VERTEX_ATTRIB_COUNT
        ; ++i )
    {
      hasTexCoords = !!pset->getVertexAttributeSet()->getNumberOfVertexData(i);
    }
    return hasTexCoords!=textured;
  }
  return false; 
}

void  WRLLoader::interpretBox( const SmartPtr<Box> & pBox, vector<PrimitiveSharedPtr> &primitives, bool textured )
{
  if (  evalTextured(pBox->pTriangles, textured)
     || evalTextured(pBox->pQuads, textured) )
  {
    pBox->pTriangles.reset();
    pBox->pQuads.reset();
  }

  if ( pBox->pTriangles || pBox->pQuads )
  {
    if ( pBox->pTriangles )
    {
      primitives.push_back( pBox->pTriangles );
    }
    if ( pBox->pQuads )
    {
      primitives.push_back( pBox->pQuads );
    }
  }
  else
  {
    SmartPtr<IndexedFaceSet> pIndexedFaceSet( new IndexedFaceSet );
    pIndexedFaceSet->setName( pBox->getName() );

    createBox( pIndexedFaceSet, pBox->size, textured );

    interpretIndexedFaceSet( pIndexedFaceSet, primitives );
    if ( pIndexedFaceSet->pTriangles )
    {
      pBox->pTriangles = pIndexedFaceSet->pTriangles;
    }
    if ( pIndexedFaceSet->pQuads )
    {
      pBox->pQuads = pIndexedFaceSet->pQuads;
    }
  }
}

void  WRLLoader::interpretCone( const SmartPtr<Cone> & pCone, vector<PrimitiveSharedPtr> &primitives, bool textured )
{
  if (  evalTextured(pCone->pTriangles, textured)
     || evalTextured(pCone->pQuads, textured) )
  {
    pCone->pTriangles.reset();
    pCone->pQuads.reset();
  }

  if ( pCone->pTriangles || pCone->pQuads )
  {
    if ( pCone->pTriangles )
    {
      primitives.push_back( pCone->pTriangles );
    }
    if ( pCone->pQuads )
    {
      primitives.push_back( pCone->pQuads );
    }
  }
  else
  {
    SmartPtr<IndexedFaceSet> pIndexedFaceSet( new IndexedFaceSet );
    pIndexedFaceSet->setName( pCone->getName() );

    createCone( pIndexedFaceSet, pCone->bottomRadius, pCone->height, 
                pCone->bottom, pCone->side, textured );

    interpretIndexedFaceSet( pIndexedFaceSet, primitives );
    if ( pIndexedFaceSet->pTriangles )
    {
      pCone->pTriangles = pIndexedFaceSet->pTriangles;
    }
    if ( pIndexedFaceSet->pQuads )
    {
      pCone->pQuads = pIndexedFaceSet->pQuads;
    }
  }
}

void  WRLLoader::interpretCylinder( const SmartPtr<Cylinder> & pCylinder, vector<PrimitiveSharedPtr> &primitives, bool textured )
{
  if (  evalTextured(pCylinder->pTriangles, textured)
     || evalTextured(pCylinder->pQuads, textured) )
  {
    pCylinder->pTriangles.reset();
    pCylinder->pQuads.reset();
  }

  if ( pCylinder->pTriangles || pCylinder->pQuads )
  {
    if ( pCylinder->pTriangles )
    {
      primitives.push_back( pCylinder->pTriangles );
    }
    if ( pCylinder->pQuads )
    {
      primitives.push_back( pCylinder->pQuads );
    }
  }
  else
  {
    SmartPtr<IndexedFaceSet> pIndexedFaceSet( new IndexedFaceSet );
    pIndexedFaceSet->setName( pCylinder->getName() );

    createCylinder( pIndexedFaceSet, pCylinder->radius, pCylinder->height, 
                    pCylinder->bottom, pCylinder->side, pCylinder->top, textured );

    interpretIndexedFaceSet( pIndexedFaceSet, primitives );
    if ( pIndexedFaceSet->pTriangles )
    {
      pCylinder->pTriangles = pIndexedFaceSet->pTriangles;
    }
    if ( pIndexedFaceSet->pQuads )
    {
      pCylinder->pQuads = pIndexedFaceSet->pQuads;
    }
  }
}

void  WRLLoader::interpretSphere( const SmartPtr<Sphere> & pSphere, vector<PrimitiveSharedPtr> &primitives, bool textured )
{
  if (  evalTextured(pSphere->pTriangles, textured)
     || evalTextured(pSphere->pQuads, textured) )
  {
    pSphere->pTriangles.reset();
    pSphere->pQuads.reset();
  }

  if ( pSphere->pTriangles || pSphere->pQuads )
  {
    if ( pSphere->pTriangles )
    {
      primitives.push_back( pSphere->pTriangles );
    }
    if ( pSphere->pQuads )
    {
      primitives.push_back( pSphere->pQuads );
    }
  }
  else
  {
    SmartPtr<IndexedFaceSet> pIndexedFaceSet( new IndexedFaceSet );
    pIndexedFaceSet->setName( pSphere->getName() );

    createSphere( pIndexedFaceSet, pSphere->radius, textured );

    interpretIndexedFaceSet( pIndexedFaceSet, primitives );
    if ( pIndexedFaceSet->pTriangles )
    {
      pSphere->pTriangles = pIndexedFaceSet->pTriangles;
    }
    if ( pIndexedFaceSet->pQuads )
    {
      pSphere->pQuads = pIndexedFaceSet->pQuads;
    }
  }
}

void  WRLLoader::interpretChildren( MFNode &children, dp::sg::core::GroupSharedPtr const& pNVSGGroup )
{
  for ( size_t i=0 ; i<children.size() ; i++ )
  {
    ObjectSharedPtr pObject = interpretSFNode( children[i] );
    if ( pObject )
    {
      DP_ASSERT( pObject.isPtrTo<Node>() );
      pNVSGGroup->addChild( pObject.staticCast<Node>() );
      // LightReferences will be added to the root with the WRLLoadTraverser !
    }
  }
}

void WRLLoader::interpretColor( const SmartPtr<Color> & pColor )
{
  if ( ! pColor->interpreted )
  {
    if ( pColor->set_color )
    {
      interpretColorInterpolator( pColor->set_color.get(), checked_cast<unsigned int>(pColor->color.size()) );
    }
    pColor->interpreted = true;
  }
}

void WRLLoader::interpretColorInterpolator( const SmartPtr<ColorInterpolator> & pColorInterpolator
                                          , unsigned int colorCount )
{
  if ( ! pColorInterpolator->interpreted )
  {
    SFTime cycleInterval = pColorInterpolator->set_fraction ? pColorInterpolator->set_fraction->cycleInterval : 1.0;
    resampleKeyValues( pColorInterpolator->key, pColorInterpolator->keyValue, colorCount
                     , pColorInterpolator->steps, cycleInterval );
    for ( size_t i=0 ; i<pColorInterpolator->keyValue.size() ; i++ )
    {
      clamp( pColorInterpolator->keyValue[i][0], 0.0f, 1.0f );
      clamp( pColorInterpolator->keyValue[i][1], 0.0f, 1.0f );
      clamp( pColorInterpolator->keyValue[i][2], 0.0f, 1.0f );
    }
    pColorInterpolator->interpreted = true;
  }
}

void WRLLoader::interpretCoordinate( const SmartPtr<Coordinate> & pCoordinate )
{
  if ( !pCoordinate->interpreted )
  {
    if ( pCoordinate->set_point )
    {
      interpretCoordinateInterpolator( pCoordinate->set_point.get()
                                     , checked_cast<unsigned int>(pCoordinate->point.size()) );
    }
    pCoordinate->interpreted = true;
  }
}

void WRLLoader::interpretCoordinateInterpolator( const SmartPtr<CoordinateInterpolator> & pCoordinateInterpolator
                                               , unsigned int pointCount )
{
  if ( ! pCoordinateInterpolator->interpreted )
  {
    SFTime cycleInterval = pCoordinateInterpolator->set_fraction ? pCoordinateInterpolator->set_fraction->cycleInterval : 1.0;
    resampleKeyValues( pCoordinateInterpolator->key, pCoordinateInterpolator->keyValue
                     , pointCount, pCoordinateInterpolator->steps, cycleInterval );
    pCoordinateInterpolator->interpreted = true;
  }
}

LightSourceSharedPtr WRLLoader::interpretDirectionalLight( const SmartPtr<DirectionalLight> & directionalLight )
{
  LightSourceSharedPtr lightSource;
  if ( directionalLight->lightSource )
  {
    lightSource = directionalLight->lightSource;
  }
  else
  {
    directionalLight->direction.normalize();

    Vec3f color( interpretSFColor( directionalLight->color ) );
    lightSource = createStandardDirectedLight( directionalLight->direction
                                             , directionalLight->ambientIntensity * color
                                             , directionalLight->intensity * color
                                             , directionalLight->intensity * color );
    lightSource->setName( directionalLight->getName() );
    lightSource->setEnabled( directionalLight->on );
  }
  return( lightSource );
}

void  WRLLoader::interpretElevationGrid( const SmartPtr<ElevationGrid> & pElevationGrid
                                       , vector<PrimitiveSharedPtr> &primitives )
{
  if ( pElevationGrid->pTriangles || pElevationGrid->pQuads )
  {
    if ( pElevationGrid->pTriangles )
    {
      primitives.push_back( pElevationGrid->pTriangles );
    }
    if ( pElevationGrid->pQuads )
    {
      primitives.push_back( pElevationGrid->pQuads );
    }
  }
  else
  {
    SmartPtr<IndexedFaceSet> pIndexedFaceSet = new IndexedFaceSet;
    pIndexedFaceSet->setName( pElevationGrid->getName() );

    Coordinate  * pCoordinate = new Coordinate;
    pCoordinate->point.reserve( pElevationGrid->height.size() );
    for ( int j=0 ; j<pElevationGrid->zDimension ; j++ )
    {
      for ( int i=0 ; i<pElevationGrid->xDimension ; i++ )
      {
        pCoordinate->point.push_back( Vec3f( pElevationGrid->xSpacing * i
                                           , pElevationGrid->height[i+j*pElevationGrid->xDimension]
                                           , pElevationGrid->zSpacing * j ) );
      }
    }
    pIndexedFaceSet->coord = pCoordinate;
    pIndexedFaceSet->coordIndex.reserve( 6 * ( pElevationGrid->xDimension - 1 ) * ( pElevationGrid->zDimension - 1 ) );
    vector<int> faceIndex;
    faceIndex.reserve( 2 * ( pElevationGrid->xDimension - 1 ) * ( pElevationGrid->zDimension - 1 ) );
    for ( int j=0 ; j<pElevationGrid->zDimension-1 ; j++ )
    {
      for ( int i=0 ; i<pElevationGrid->xDimension-1 ; i++ )
      {
        pIndexedFaceSet->coordIndex.push_back(  j    * pElevationGrid->xDimension + i     );
        pIndexedFaceSet->coordIndex.push_back( (j+1) * pElevationGrid->xDimension + i     );
        pIndexedFaceSet->coordIndex.push_back( (j+1) * pElevationGrid->xDimension + i + 1 );
        pIndexedFaceSet->coordIndex.push_back(  j    * pElevationGrid->xDimension + i + 1 );
        pIndexedFaceSet->coordIndex.push_back( -1 );
        faceIndex.push_back( j * pElevationGrid->xDimension + i );
      }
    }

    if ( pElevationGrid->texCoord )
    {
      pIndexedFaceSet->texCoord = pElevationGrid->texCoord;
    }
    else
    {
      TextureCoordinate * pTextureCoordinate = new TextureCoordinate;
      pTextureCoordinate->point.reserve( pElevationGrid->height.size() );
      float xStep = 1.0f / pElevationGrid->xDimension;
      float zStep = 1.0f / pElevationGrid->zDimension;
      for ( int j=0 ; j<pElevationGrid->zDimension ; j++ )
      {
        for ( int i=0 ; i<pElevationGrid->xDimension ; i++ )
        {
          pTextureCoordinate->point.push_back( Vec2f( i * xStep, j * zStep ) );
        }
      }
      pIndexedFaceSet->texCoord = pTextureCoordinate;
    }

    if ( pElevationGrid->color )
    {
      pIndexedFaceSet->color = pElevationGrid->color;
      pIndexedFaceSet->colorPerVertex = pElevationGrid->colorPerVertex;
      if ( ! pIndexedFaceSet->colorPerVertex )
      {
        pIndexedFaceSet->colorIndex = faceIndex;
      }
    }
    if ( pElevationGrid->normal )
    {
      pIndexedFaceSet->normal = pElevationGrid->normal;
      pIndexedFaceSet->normalPerVertex = pElevationGrid->normalPerVertex;
      if ( ! pIndexedFaceSet->normalPerVertex )
      {
        pIndexedFaceSet->normalIndex = faceIndex;
      }
    }
    pIndexedFaceSet->ccw = pElevationGrid->ccw;
    pIndexedFaceSet->creaseAngle = pElevationGrid->creaseAngle;
    pIndexedFaceSet->solid = pElevationGrid->solid;

    interpretIndexedFaceSet( pIndexedFaceSet, primitives );
    if ( pIndexedFaceSet->pTriangles )
    {
      pElevationGrid->pTriangles = pIndexedFaceSet->pTriangles;
    }
    if ( pIndexedFaceSet->pQuads )
    {
      pElevationGrid->pQuads     = pIndexedFaceSet->pQuads;
    }
  }
}

void  WRLLoader::interpretGeometry( const SmartPtr<vrml::Geometry> & pGeometry, vector<PrimitiveSharedPtr> &primitives
                                  , bool textured )
{
  if ( isSmartPtrOf<Box>(pGeometry) )
  {
    interpretBox( smart_cast<Box>(pGeometry), primitives, textured );
  }
  else if ( isSmartPtrOf<Cone>(pGeometry) )
  {
    interpretCone( smart_cast<Cone>(pGeometry), primitives, textured );
  }
  else if ( isSmartPtrOf<Cylinder>(pGeometry) )
  {
    interpretCylinder( smart_cast<Cylinder>(pGeometry), primitives, textured );
  }
  else if ( isSmartPtrOf<ElevationGrid>(pGeometry) )
  {
    interpretElevationGrid( smart_cast<ElevationGrid>(pGeometry), primitives );
  }
  else if ( isSmartPtrOf<IndexedFaceSet>(pGeometry) )
  {
    interpretIndexedFaceSet( smart_cast<IndexedFaceSet>(pGeometry), primitives );
  }
  else if ( isSmartPtrOf<IndexedLineSet>(pGeometry) )
  {
    interpretIndexedLineSet( smart_cast<IndexedLineSet>(pGeometry), primitives );
  }
  else if ( isSmartPtrOf<Sphere>(pGeometry) )
  {
    interpretSphere( smart_cast<Sphere>(pGeometry), primitives, textured );
  }
  else
  {
    DP_ASSERT( isSmartPtrOf<PointSet>(pGeometry) );
    interpretPointSet( smart_cast<PointSet>(pGeometry), primitives );
  }
}

GroupSharedPtr WRLLoader::interpretGroup( const SmartPtr<vrml::Group> & pGroup )
{
  GroupSharedPtr pNVSGGroup;
  if ( pGroup->pGroup )
  {
    pNVSGGroup = pGroup->pGroup;
  }
  else
  {
    pNVSGGroup = dp::sg::core::Group::create();
    pNVSGGroup->setName( pGroup->getName() );
    interpretChildren( pGroup->children, pNVSGGroup );
    pGroup->pGroup = pNVSGGroup;
  }

  return( pNVSGGroup );
}

ParameterGroupDataSharedPtr WRLLoader::interpretImageTexture( const SmartPtr<ImageTexture> & pImageTexture )
{
  if ( !pImageTexture->textureData )
  {
    string  fileName;
    if ( interpretURL( pImageTexture->url, fileName ) )
    {
      map<string,TextureHostWeakPtr>::const_iterator it = m_textureFiles.find( fileName );
      TextureHostSharedPtr texImg;
      if ( it == m_textureFiles.end() )
      {
        texImg = dp::sg::io::loadTextureHost(fileName, m_searchPaths);
        DP_ASSERT( texImg );
        texImg->setTextureTarget(TT_TEXTURE_2D); // TEXTURE_2D is the only target known by VRML
        m_textureFiles[fileName] = texImg.getWeakPtr();
      }
      else
      {
        texImg = it->second->getSharedPtr<TextureHost>();
      }

      SamplerSharedPtr sampler = Sampler::create( texImg );
      sampler->setWrapMode( TWCA_S, pImageTexture->repeatS ? TWM_REPEAT : TWM_CLAMP_TO_EDGE );
      sampler->setWrapMode( TWCA_T, pImageTexture->repeatT ? TWM_REPEAT : TWM_CLAMP_TO_EDGE );

      pImageTexture->textureData = createStandardTextureParameterData( sampler );
      pImageTexture->textureData->setName( pImageTexture->getName() );
    }
  }
  return( pImageTexture->textureData );
}

void  analyzeIndex( const MFInt32 & mfInt32, vector<unsigned int> & triVerts, vector<unsigned int> & triFaces
                  , vector<unsigned int> & quadVerts, vector<unsigned int> & quadFaces
                  , vector<unsigned int> & polygonVerts, vector<unsigned int> & polygonFaces )
{
  triVerts.clear();
  triFaces.clear();
  quadVerts.clear();
  quadFaces.clear();

  unsigned int faceIndex = 0;
  unsigned int endIndex  = 0;
  do
  {
    unsigned int startIndex = endIndex;
    for ( ; endIndex<mfInt32.size() && mfInt32[endIndex]!=-1 ; endIndex++ )
      ;
    endIndex++;
    switch( endIndex - startIndex )
    {
      case 0 :
      case 1 :
      case 2 :
      case 3 :
        DP_ASSERT( false );
        break;
      case 4 :
        triVerts.push_back( startIndex );
        triFaces.push_back( faceIndex );
        break;
      case 5 :
        quadVerts.push_back( startIndex );
        quadFaces.push_back( faceIndex );
        break;
      default :
        polygonVerts.push_back( startIndex );
        polygonFaces.push_back( faceIndex );
        break;
    }
    faceIndex++;
  } while ( endIndex < mfInt32.size() );
}

void  WRLLoader::interpretIndexedFaceSet( const SmartPtr<IndexedFaceSet> & pIndexedFaceSet
                                        , vector<PrimitiveSharedPtr> &primitives )
{
  DP_ASSERT( pIndexedFaceSet->coord );

  if ( pIndexedFaceSet->pTriangles || pIndexedFaceSet->pQuads || pIndexedFaceSet->pPolygons )
  {
    if ( pIndexedFaceSet->pTriangles )
    {
      primitives.push_back( pIndexedFaceSet->pTriangles );
    }
    if ( pIndexedFaceSet->pQuads )
    {
      primitives.push_back( pIndexedFaceSet->pQuads );
    }
    if ( pIndexedFaceSet->pPolygons )
    {
      primitives.push_back( pIndexedFaceSet->pPolygons );
    }
  }
  else if ( pIndexedFaceSet->coordIndex.size() )
  {
    if ( pIndexedFaceSet->color )
    {
      DP_ASSERT( isSmartPtrOf<Color>(pIndexedFaceSet->color) );
      interpretColor( smart_cast<Color>(pIndexedFaceSet->color) );
    }
    if ( pIndexedFaceSet->coord )
    {
      DP_ASSERT( isSmartPtrOf<Coordinate>(pIndexedFaceSet->coord) );
      interpretCoordinate( smart_cast<Coordinate>(pIndexedFaceSet->coord) );
    }
    if ( pIndexedFaceSet->normal )
    {
      DP_ASSERT( isSmartPtrOf<Normal>(pIndexedFaceSet->normal) );
      interpretNormal( smart_cast<Normal>(pIndexedFaceSet->normal) );
    }
    // no need to interpret texCoord

    //  determine the triangles, quads and polygon faces
    vector<unsigned int> triVerts, triFaces, quadVerts, quadFaces, polygonVerts, polygonFaces;
    analyzeIndex( pIndexedFaceSet->coordIndex, triVerts, triFaces, quadVerts, quadFaces, polygonVerts, polygonFaces );

    GroupSharedPtr smoothGroup;
    if ( !pIndexedFaceSet->normal )
    {
      smoothGroup = dp::sg::core::Group::create();
    }
    if ( triFaces.size() )
    {
      VertexAttributeSetSharedPtr vas = interpretVertexAttributeSet( pIndexedFaceSet
                                                                   , checked_cast<unsigned int>(3*triFaces.size())
                                                                   , triVerts, triFaces );

      //  create the Triangles
      PrimitiveSharedPtr pTriangles = Primitive::create( PRIMITIVE_TRIANGLES );
      pTriangles->setName( pIndexedFaceSet->getName() );
      pTriangles->setVertexAttributeSet( vas );
      if ( !pIndexedFaceSet->normal )
      {
        pTriangles->generateNormals();
        GeoNodeSharedPtr smoothGeoNode = GeoNode::create();
        smoothGeoNode->setPrimitive( pTriangles );
        smoothGroup->addChild( smoothGeoNode );
      }

      primitives.push_back( pTriangles );
      pIndexedFaceSet->pTriangles = pTriangles;
    }

    if ( quadFaces.size() )
    {
      VertexAttributeSetSharedPtr vas = interpretVertexAttributeSet( pIndexedFaceSet
                                                                   , checked_cast<unsigned int>(4*quadFaces.size())
                                                                   , quadVerts, quadFaces );

      //  create the Quads
      PrimitiveSharedPtr pQuads = Primitive::create( PRIMITIVE_QUADS );
      pQuads->setName( pIndexedFaceSet->getName() );
      pQuads->setVertexAttributeSet( vas );
      if ( !pIndexedFaceSet->normal )
      {
        pQuads->generateNormals();
        GeoNodeSharedPtr smoothGeoNode = GeoNode::create();
        smoothGeoNode->setPrimitive( pQuads );
        smoothGroup->addChild( smoothGeoNode );
      }

      primitives.push_back( pQuads );
      pIndexedFaceSet->pQuads = pQuads;
    }

    if ( polygonVerts.size() )
    {
      // create the IndexSet
      vector<unsigned int> indices;
      unsigned int  numberOfVertices = 0;
      for ( size_t i=0 ; i<polygonVerts.size() ; i++ )
      {
        for ( size_t j=0 ; pIndexedFaceSet->coordIndex[polygonVerts[i]+j] != -1 ; j++ )
        {
          indices.push_back( numberOfVertices++ );
        }
        indices.push_back( ~0 );
      }
      indices.pop_back();
      IndexSetSharedPtr is = IndexSet::create();
      is->setData( &indices[0], checked_cast<unsigned int>( indices.size() ) );

      // create the VertexAttributeSet
      VertexAttributeSetSharedPtr vas = interpretVertexAttributeSet( pIndexedFaceSet, numberOfVertices
                                                                   , polygonVerts, polygonFaces );

      // create the polygons Primitive
      PrimitiveSharedPtr pPolygons = Primitive::create( PRIMITIVE_POLYGON );
      pPolygons->setName( pIndexedFaceSet->getName() );
      pPolygons->setIndexSet( is );
      pPolygons->setVertexAttributeSet( vas );
      if ( !pIndexedFaceSet->normal )
      {
        pPolygons->generateNormals();
        // don't smooth the normals of a polygon!
      }

      primitives.push_back( pPolygons );
      pIndexedFaceSet->pPolygons = pPolygons;
    }

    if ( !pIndexedFaceSet->normal && ( smoothGroup->getNumberOfChildren() != 0 ) )
    {
      m_smoothTraverser->setCreaseAngle( pIndexedFaceSet->creaseAngle );
      m_smoothTraverser->apply( NodeSharedPtr( smoothGroup ) );
    }
  }
}

void gatherVec3fPerFace( vector<Vec3f> &vTo, const MFVec3f &vFrom, const MFInt32 & index
                       , unsigned int numberOfVertices, unsigned int fromOff, const vector<unsigned int> & startIndices
                       , const vector<unsigned int> & faceIndices )
{
  DP_ASSERT( false );   // never encountered this path
  DP_ASSERT( startIndices.size() == faceIndices.size() );
  vTo.resize( numberOfVertices );
  for ( size_t i=0, idx=0 ; i<startIndices.size() ; i++ )
  {
    Vec3f v = vFrom[fromOff+faceIndices[i]];
    for ( size_t j=0 ; index[startIndices[i]+j] != -1 ; j++ )
    {
      DP_ASSERT( idx < numberOfVertices );
      vTo[idx++] = v;
    }
  }
}

void  gatherVec3fPerFaceIndexed( vector<Vec3f> & vTo, const MFVec3f & vFrom, const MFInt32 & vertexIndex
                               , const MFInt32 & faceIndex, unsigned int numberOfVertices, unsigned int fromOff
                               , const vector<unsigned int> & startIndices, const vector<unsigned int> & faceIndices )
{
  DP_ASSERT( startIndices.size() == faceIndices.size() );
  vTo.resize( numberOfVertices );
  for ( size_t i=0, idx=0 ; i<startIndices.size() ; i++ )
  {
    Vec3f v = vFrom[fromOff+faceIndex[faceIndices[i]]];
    for ( size_t j=0 ; vertexIndex[startIndices[i]+j] != -1 ; j++ )
    {
      DP_ASSERT( idx < numberOfVertices );
      vTo[idx++] = v;
    }
  }
}

template<typename T>
void gatherPerVertex( vector<T> &vTo, const vector<T> & vFrom, const MFInt32 & index
                    , unsigned int numberOfVertices, unsigned int fromOff
                    , const vector<unsigned int> & startIndices, bool ccw )
{
  vTo.resize( numberOfVertices );
  for ( size_t i=0, idx=0 ; i<startIndices.size() ; i++ )
  {
    if ( ccw )
    {
      for ( size_t j=0 ; index[startIndices[i]+j] != -1 ; j++ )
      {
        DP_ASSERT( idx < numberOfVertices );
        vTo[idx++] = vFrom[fromOff+index[startIndices[i]+j]];
      }
    }
    else
    {
      size_t lastIdx = 0;
      for ( ; index[startIndices[i]+lastIdx] != -1 ; lastIdx++ )
        ;
      DP_ASSERT( index[startIndices[i]+lastIdx] == -1 );
      for ( size_t j=lastIdx-1 ; j<=lastIdx ; j-- )   // use wrap-around from 0 "down" to size_t max to end this loop
      {
        DP_ASSERT( idx < numberOfVertices );
        vTo[idx++] = vFrom[fromOff+index[startIndices[i]+j]];
      }
    }
  }
}

template<typename T>
void WRLLoader::resampleKeyValues( MFFloat & keys, vector<T> & values, unsigned int valuesPerKey
                                 , vector<unsigned int> & steps, SFTime cycleInterval )
{
  DP_ASSERT( ! keys.empty() );
  //          step                used keys
  vector<pair<unsigned int,pair<unsigned int,unsigned int> > > stepMap;

  float stepSize = (float)( keys.back() / ( m_stepsPerUnit * cycleInterval ) );
  float halfStepSize = 0.5f * stepSize;

  //  the first key is used as the first step, no matter when it is...
  stepMap.push_back( make_pair(0,make_pair(0,0)) );
  unsigned int startIndex = 0;
  if ( keys[0] < halfStepSize )
  {
    //  first key starts at 0 -> skip any key in the first halfStepSize interval
    while ( startIndex < keys.size() && keys[startIndex] < halfStepSize )
    {
      ++startIndex;
    }
  }

  while ( startIndex < keys.size() )
  {
    // start a new key step
    unsigned int step = static_cast<unsigned int>(( keys[startIndex] + halfStepSize ) / stepSize);
    DP_ASSERT( stepMap.back().first < step );
    float stepPos = step * stepSize;

    if ( abs( stepPos - keys[startIndex] ) < FLT_EPSILON )
    {
      // key coincides with step position -> use just that key
      stepMap.push_back( make_pair(step,make_pair(startIndex,0)) );
    }
    else if ( stepPos < keys[startIndex] )
    {
      // key is to the right of the step position -> use previous and this key
      stepMap.push_back( make_pair(step,make_pair(startIndex-1,startIndex)) );
    }
    else
    {
      // key is to the left of the step position -> scan for the first key to the right
      while ( ( startIndex+1 < keys.size() ) && ( keys[startIndex+1] < stepPos - FLT_EPSILON ) )
      {
        startIndex++;
      }
      if ( startIndex + 1 < keys.size() )
      {
        if ( abs( stepPos - keys[startIndex+1] ) < FLT_EPSILON )
        {
          // next key coincides with step position -> use just that key
          stepMap.push_back( make_pair(step,make_pair(startIndex+1,0)) );
        }
        else 
        {
          // -> use startIndex and next
          stepMap.push_back( make_pair(step,make_pair(startIndex,startIndex+1)) );
        }
      }
      else
      {
        // startIndex is the last key (and to the left of step position), so just use it
        stepMap.push_back( make_pair(step,make_pair(startIndex,0)) );
      }
    }
    // skip keys in that same step
    while ( ( startIndex < keys.size() ) && ( keys[startIndex] <= stepPos + halfStepSize ) )
    {
      startIndex++;
    }
  }

  //  now, size of stepMap gives the number of needed keys, with stepMap[i].first being the step
  //  index and stepMap[i].second.first/second the two value to interpolate (if second.second is
  //  zero, just use second.first)
  MFFloat keysOut;
  keysOut.reserve( stepMap.size() );
  vector<T> valuesOut;
  valuesOut.reserve( stepMap.size() * valuesPerKey );
  steps.reserve( stepMap.size() );

  //  create one set of data per stepMap entry
  for ( size_t i=0 ; i<stepMap.size() ; i++ )
  {
    // store the step index to use in steps, and the corresponding key in keysOut
    steps.push_back( stepMap[i].first );
    keysOut.push_back( stepMap[i].first * stepSize );

    if ( stepMap[i].second.second == 0 )
    {
      // there's only one key in that step -> copy it over
      unsigned int idx = stepMap[i].second.first * valuesPerKey;
      valuesOut.insert( valuesOut.end(), values.begin()+idx, values.begin()+idx+valuesPerKey );
    }
    else
    {
      // there are two keys -> calculate the weighted sum
      float keyStep = stepMap[i].first * stepSize;
      DP_ASSERT(    ( keys[stepMap[i].second.first] < keyStep )
                  &&  ( keyStep < keys[stepMap[i].second.second] ) );
      float dist = keys[stepMap[i].second.second] - keys[stepMap[i].second.first];
      DP_ASSERT( FLT_EPSILON < dist );
      float alpha = ( keyStep - keys[stepMap[i].second.first] ) / dist;

      unsigned int idx0 = stepMap[i].second.first * valuesPerKey;
      unsigned int idx1 = stepMap[i].second.second * valuesPerKey;
      DP_ASSERT( idx1 - idx0 == valuesPerKey );
      for ( unsigned int k=0 ; k<valuesPerKey ; k++ )
      {
        valuesOut.push_back( lerp( alpha, values[idx0+k], values[idx1+k] ) );
      }
    }
  }

  // swap keys and values now!
  keys.swap( keysOut );
  values.swap( valuesOut );
}

VertexAttributeSetSharedPtr WRLLoader::interpretVertexAttributeSet( const SmartPtr<IndexedFaceSet> & pIndexedFaceSet
                                                                  , unsigned int numberOfVertices
                                                                  , const vector<unsigned int> & startIndices
                                                                  , const vector<unsigned int> & faceIndices )
{
  DP_ASSERT( isSmartPtrOf<Coordinate>(pIndexedFaceSet->coord) );
  const Coordinate * pCoordinate = static_cast<const Coordinate *>(pIndexedFaceSet->coord.get());
  const Normal     * pNormal     = static_cast<const Normal     *>(pIndexedFaceSet->normal.get());
  const Color      * pColor      = static_cast<const Color      *>(pIndexedFaceSet->color.get());

  VertexAttributeSetSharedPtr vash;
#if defined(KEEP_ANIMATION)
  if ( pCoordinate->set_point || ( pNormal && pNormal->set_vector ) || ( pColor && pColor->set_color ) )
  {
    AnimatedVertexAttributeSetSharedPtr avash = AnimatedVertexAttributeSet::create();
    //  set the animated vertices
    if ( pCoordinate->set_point )
    {
      DP_ASSERT( pCoordinate->interpreted );
      LinearInterpolatedVertexAttributeAnimationDescriptionSharedPtr livaadh = LinearInterpolatedVertexAttributeAnimationDescription::create();
      {
        LinearInterpolatedVertexAttributeAnimationDescriptionLock liadva(livaadh);
        unsigned int keyCount = checked_cast<unsigned int>(pCoordinate->set_point->key.size());
        liadva->reserveKeys( keyCount );

        vector<Vec3f> vertices;
        unsigned int pointCount = checked_cast<unsigned int>(pCoordinate->point.size());
        for ( unsigned int i=0 ; i<keyCount ; i++ )
        {
          gatherPerVertex<Vec3f>( vertices, pCoordinate->set_point->keyValue, pIndexedFaceSet->coordIndex
                                , numberOfVertices, i*pointCount, startIndices, pIndexedFaceSet->ccw );
          VertexAttribute va;
          va.setData( 3, dp::util::DT_FLOAT_32, &vertices[0], 0, checked_cast<unsigned int>(vertices.size()) );
          liadva->addKey( pCoordinate->set_point->steps[i], va );
        }
      }
      VertexAttributeAnimationSharedPtr vaah = VertexAttributeAnimation::create();
      VertexAttributeAnimationLock(vaah)->setDescription( livaadh );
      {
        AnimatedVertexAttributeSetLock avas(avash);
        avas->setAnimation( VertexAttributeSet::NVSG_POSITION, vaah );
      }
    }

    //  set the animated normals
    if ( pNormal && pNormal->set_vector )
    {
      DP_ASSERT( pNormal->interpreted );
      DP_ASSERT( pCoordinate->set_point->key == pNormal->set_vector->key );

      LinearInterpolatedVertexAttributeAnimationDescriptionSharedPtr livaadh = LinearInterpolatedVertexAttributeAnimationDescription::create();
      {
        LinearInterpolatedVertexAttributeAnimationDescriptionLock liadva(livaadh);
        unsigned int keyCount = checked_cast<unsigned int>(pNormal->set_vector->key.size());
        liadva->reserveKeys( keyCount );

        vector<Vec3f> normals;
        unsigned int normalCount = checked_cast<unsigned int>(pNormal->vector.size());
        if ( pIndexedFaceSet->normalPerVertex )
        {
          if ( pIndexedFaceSet->normalIndex.empty() )
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherPerVertex<Vec3f>( normals, pNormal->set_vector->keyValue, pIndexedFaceSet->coordIndex
                                    , numberOfVertices, i*normalCount, startIndices, pIndexedFaceSet->ccw );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &normals[0], 0, checked_cast<unsigned int>(normals.size()) );
              liadva->addKey( pNormal->set_vector->steps[i], va );
            }
          }
          else
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherPerVertex<Vec3f>( normals, pNormal->set_vector->keyValue, pIndexedFaceSet->normalIndex
                                    , numberOfVertices, i*normalCount, startIndices, pIndexedFaceSet->ccw );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &normals[0], 0, checked_cast<unsigned int>(normals.size()) );
              liadva->addKey( pNormal->set_vector->steps[i], va );
            }
          }
        }
        else
        {
          if ( pIndexedFaceSet->normalIndex.empty() )
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherVec3fPerFace( normals, pNormal->set_vector->keyValue, pIndexedFaceSet->coordIndex
                                , numberOfVertices, i*normalCount, startIndices, faceIndices );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &normals[0], 0, checked_cast<unsigned int>(normals.size()) );
              liadva->addKey( pNormal->set_vector->steps[i], va );
            }
          }
          else
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherVec3fPerFaceIndexed( normals, pNormal->set_vector->keyValue, pIndexedFaceSet->coordIndex
                                       , pIndexedFaceSet->normalIndex, numberOfVertices
                                       , i*normalCount, startIndices, faceIndices );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &normals[0], 0, checked_cast<unsigned int>(normals.size()) );
              liadva->addKey( pNormal->set_vector->steps[i], va );
            }
          }
        }
      }
      VertexAttributeAnimationSharedPtr vaah = VertexAttributeAnimation::create();
      VertexAttributeAnimationLock(vaah)->setDescription( livaadh );
      {
        AnimatedVertexAttributeSetLock avas(avash);
        avas->setAnimation( VertexAttributeSet::NVSG_NORMAL, vaah );
      }
    }

    //  set the animated colors
    if ( pColor && pColor->set_color )
    {
      DP_ASSERT( pColor->interpreted );
      DP_ASSERT( pCoordinate->set_point->key == pColor->set_color->key );

      LinearInterpolatedVertexAttributeAnimationDescriptionSharedPtr livaadh = LinearInterpolatedVertexAttributeAnimationDescription::create();
      {
        LinearInterpolatedVertexAttributeAnimationDescriptionLock liadva(livaadh);
        unsigned int keyCount = checked_cast<unsigned int>(pColor->set_color->key.size());
        liadva->reserveKeys( keyCount );

        vector<Vec3f> colors;
        unsigned int colorCount = checked_cast<unsigned int>(pColor->color.size());
        if ( pIndexedFaceSet->colorPerVertex )
        {
          if ( pIndexedFaceSet->colorIndex.empty() )
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherPerVertex<Vec3f>( colors, pColor->set_color->keyValue, pIndexedFaceSet->coordIndex
                                    , numberOfVertices, i*colorCount, startIndices, pIndexedFaceSet->ccw );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &colors[0], 0, checked_cast<unsigned int>(colors.size()) );
              liadva->addKey( pColor->set_color->steps[i], va );
            }
          }
          else
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherPerVertex<Vec3f>( colors, pColor->set_color->keyValue, pIndexedFaceSet->colorIndex
                                    , numberOfVertices, i*colorCount, startIndices, pIndexedFaceSet->ccw );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &colors[0], 0, checked_cast<unsigned int>(colors.size()) );
              liadva->addKey( pColor->set_color->steps[i], va );
            }
          }
        }
        else
        {
          if ( pIndexedFaceSet->colorIndex.empty() )
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherVec3fPerFace( colors, pColor->set_color->keyValue, pIndexedFaceSet->coordIndex
                                , numberOfVertices, i*colorCount, startIndices, faceIndices );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &colors[0], 0, checked_cast<unsigned int>(colors.size()) );
              liadva->addKey( pColor->set_color->steps[i], va );
            }
          }
          else
          {
            for ( unsigned int i=0 ; i<keyCount ; i++ )
            {
              gatherVec3fPerFaceIndexed( colors, pColor->set_color->keyValue, pIndexedFaceSet->coordIndex
                                       , pIndexedFaceSet->colorIndex, numberOfVertices
                                       , i*colorCount, startIndices, faceIndices );
              VertexAttribute va;
              va.setData( 3, dp::util::DT_FLOAT_32, &colors[0], 0, checked_cast<unsigned int>(colors.size()) );
              liadva->addKey( pColor->set_color->steps[i], va );
            }
          }
        }
      }
      VertexAttributeAnimationSharedPtr vaah = VertexAttributeAnimation::create();
      VertexAttributeAnimationLock(vaah)->setDescription( livaadh );
      {
        AnimatedVertexAttributeSetLock avas(avash);
        avas->setAnimation( VertexAttributeSet::NVSG_COLOR, vaah );
      }
    }
    vash = avash;
  }
  else
#endif
  {
    vash = VertexAttributeSet::create();
  }

  {
    //  set the vertices
    vector<Vec3f> vertices;
    gatherPerVertex<Vec3f>( vertices, pCoordinate->point, pIndexedFaceSet->coordIndex
                          , numberOfVertices, 0, startIndices, pIndexedFaceSet->ccw );
    vash->setVertices( &vertices[0], numberOfVertices );

    //  set the normals
    if ( pNormal )
    {
      vector<Vec3f> normals;
      if ( pIndexedFaceSet->normalPerVertex )
      {
        gatherPerVertex<Vec3f>( normals, pNormal->vector,
                                pIndexedFaceSet->normalIndex.empty()
                                ? pIndexedFaceSet->coordIndex
                                : pIndexedFaceSet->normalIndex,
                                numberOfVertices, 0, startIndices, pIndexedFaceSet->ccw );
      }
      else
      {
        if ( pIndexedFaceSet->normalIndex.empty() )
        {
          DP_ASSERT( false );
          gatherVec3fPerFace( normals, pNormal->vector, pIndexedFaceSet->coordIndex, numberOfVertices, 0, startIndices
                            , faceIndices );
        }
        else
        {
          gatherVec3fPerFaceIndexed( normals, pNormal->vector, pIndexedFaceSet->coordIndex, pIndexedFaceSet->normalIndex
                                   , numberOfVertices, 0, startIndices, faceIndices );
        }
      }
      vash->setNormals( &normals[0], numberOfVertices );
    }

    //  set the texture coordinates
    const TextureCoordinate * pTextureCoordinate = dynamic_cast<const TextureCoordinate *>(pIndexedFaceSet->texCoord.get());
    if ( pTextureCoordinate )
    {
      vector<Vec2f> texCoords;
      gatherPerVertex<Vec2f>( texCoords, pTextureCoordinate->point,
                              pIndexedFaceSet->texCoordIndex.empty()
                              ? pIndexedFaceSet->coordIndex
                              : pIndexedFaceSet->texCoordIndex,
                              numberOfVertices, 0, startIndices, pIndexedFaceSet->ccw );
      vash->setTexCoords( 0, &texCoords[0], numberOfVertices );
    }

    //  set the colors
    const Color * pColor = dynamic_cast<const Color *>(pIndexedFaceSet->color.get());
    if ( pColor )
    {
      vector<Vec3f> colors;
      if ( pIndexedFaceSet->colorPerVertex )
      {
        gatherPerVertex<Vec3f>( colors, pColor->color,
                                pIndexedFaceSet->colorIndex.empty()
                                ? pIndexedFaceSet->coordIndex
                                : pIndexedFaceSet->colorIndex,
                                numberOfVertices, 0, startIndices, pIndexedFaceSet->ccw );
      }
      else
      {
        if ( pIndexedFaceSet->colorIndex.empty() )
        {
          DP_ASSERT( false );
          gatherVec3fPerFace( colors, pColor->color, pIndexedFaceSet->coordIndex, numberOfVertices, 0, startIndices
                            , faceIndices );
        }
        else
        {
          gatherVec3fPerFaceIndexed( colors, pColor->color, pIndexedFaceSet->coordIndex, pIndexedFaceSet->colorIndex
                                   , numberOfVertices, 0, startIndices, faceIndices );
        }
      }
      vash->setColors( &colors[0], numberOfVertices );
    }
  }

  return( vash );
}

void  WRLLoader::interpretIndexedLineSet( const SmartPtr<IndexedLineSet> & pIndexedLineSet
                                        , vector<PrimitiveSharedPtr> &primitives )
{
  DP_ASSERT( pIndexedLineSet->coord );

  if ( pIndexedLineSet->pLineStrips )
  {
    primitives.push_back( pIndexedLineSet->pLineStrips );
  }
  else if ( pIndexedLineSet->coordIndex.size() )
  {
    vector<unsigned int> indices( pIndexedLineSet->coordIndex.size() );
    unsigned int ic = 0;
    for ( size_t i=0 ; i<pIndexedLineSet->coordIndex.size() ; i++ )
    {
      indices[i] = ( pIndexedLineSet->coordIndex[i] == -1 ) ? ~0 : ic++;
    }
    if ( indices.back() == ~0 )
    {
      indices.pop_back();
    }
    IndexSetSharedPtr iset( IndexSet::create() );
    iset->setData( &indices[0] , checked_cast<unsigned int>(indices.size()) );

    DP_ASSERT( dynamic_cast<const Coordinate *>(pIndexedLineSet->coord.get()) );
    const Coordinate * pCoordinate = static_cast<const Coordinate *>(pIndexedLineSet->coord.get());
    DP_ASSERT( pCoordinate && ! pCoordinate->set_point );
    vector<Vec3f> vertices( ic );
    for ( size_t i=0, j=0 ; i<pIndexedLineSet->coordIndex.size() ; i++ )
    {
      if ( pIndexedLineSet->coordIndex[i] != -1 )
      {
        vertices[j++] = pCoordinate->point[pIndexedLineSet->coordIndex[i]];
      }
    }
    VertexAttributeSetSharedPtr cvas = VertexAttributeSet::create();
    cvas->setVertices( &vertices[0], checked_cast<unsigned int>(vertices.size()) );

    const Color * pColor = static_cast<const Color *>(pIndexedLineSet->color.get());
    if ( pColor )
    {
      vector<Vec3f> colors( vertices.size() );
      if ( pIndexedLineSet->colorPerVertex )
      {
        MFInt32 &colorIndices = pIndexedLineSet->colorIndex.empty()
                                ? pIndexedLineSet->coordIndex
                                : pIndexedLineSet->colorIndex;
        for ( size_t i=0, j=0 ; i<colorIndices.size() ; i++ )
        {
          if ( colorIndices[i] != -1 )
          {
            colors[j++] = pColor->color[colorIndices[i]];
          }
        }
      }
      else
      {
        if ( pIndexedLineSet->colorIndex.empty() )
        {
          for ( size_t i=0, j=0, k=0 ; i<pIndexedLineSet->coordIndex.size() ; i++ )
          {
            if ( pIndexedLineSet->coordIndex[i] == -1 )
            {
              k++;
            }
            else
            {
              colors[j++] = pColor->color[k];
            }
          }
        }
        else
        {
          for ( size_t i=0, j=0, k=0 ; i<pIndexedLineSet->coordIndex.size() ; i++ )
          {
            if ( pIndexedLineSet->coordIndex[i] == -1 )
            {
              k++;
            }
            else
            {
              colors[j++] = pColor->color[pIndexedLineSet->colorIndex[k]];
            }
          }
        }
      }
      cvas->setColors( &colors[0], checked_cast<unsigned int>(colors.size()) );
    }
    
    PrimitiveSharedPtr pLineStrips = Primitive::create( PRIMITIVE_LINE_STRIP );
    pLineStrips->setName( pIndexedLineSet->getName() );
    pLineStrips->setIndexSet( iset );
    pLineStrips->setVertexAttributeSet( cvas );
    primitives.push_back( pLineStrips );
    pIndexedLineSet->pLineStrips = pLineStrips;
  }
}

NodeSharedPtr WRLLoader::interpretInline( const SmartPtr<Inline> & pInline )
{
  if ( ! pInline->pNode )
  {
    string  fileName;
    if ( interpretURL( pInline->url, fileName ) )
    {
      WRLLoader * pWRLLoader = new WRLLoader;
      dp::sg::ui::ViewStateSharedPtr viewState;
      SceneSharedPtr pScene = pWRLLoader->load( fileName, m_searchPaths, viewState );
      pInline->pNode = pScene->getRootNode();
      delete pWRLLoader;
    }
  }
  return( pInline->pNode );
}

LODSharedPtr WRLLoader::interpretLOD( const SmartPtr<vrml::LOD> & pVRMLLOD )
{
  LODSharedPtr pNVSGLOD;

  if ( pVRMLLOD->pLOD )
  {
    pNVSGLOD = pVRMLLOD->pLOD;
  }
  else
  {
    pNVSGLOD = dp::sg::core::LOD::create();
    pNVSGLOD->setName( pVRMLLOD->getName() );
    interpretChildren( pVRMLLOD->children, pNVSGLOD );
    if ( pVRMLLOD->range.size() != 0 )
    {
      pNVSGLOD->setRanges( &pVRMLLOD->range[0], checked_cast<unsigned int>(pVRMLLOD->range.size()) );
    }
    else
    {
      vector<float> ranges( pNVSGLOD->getNumberOfChildren() - 1 );
      float dist = 0.0f;
      for ( dp::sg::core::Group::ChildrenIterator gci = pNVSGLOD->beginChildren() ; gci != pNVSGLOD->endChildren() ; ++gci )
      {
        DP_ASSERT( dp::math::isValid((*gci)->getBoundingSphere()) );
        float radius = (*gci)->getBoundingSphere().getRadius();
        if ( dist < radius )
        {
          dist = radius;
        }
      }
      dist *= 10.0f;
      for ( size_t i=0 ; i<ranges.size() ; i++, dist*=10.0f )
      {
        ranges[i] = dist;
      }
      pNVSGLOD->setRanges( &ranges[0], checked_cast<unsigned int>(ranges.size()) );
    }
    pNVSGLOD->setCenter( pVRMLLOD->center );

    pVRMLLOD->pLOD = pNVSGLOD;
  }

  return( pNVSGLOD );
}

ParameterGroupDataSharedPtr WRLLoader::interpretMaterial( const SmartPtr<vrml::Material> & material )
{
  ParameterGroupDataSharedPtr materialParameters;
  if ( ! material->materialParameters )
  {
    const dp::fx::SmartEffectSpec & es = getStandardMaterialSpec();
    dp::fx::EffectSpec::iterator pgsit = es->findParameterGroupSpec( string( "standardMaterialParameters" ) );
    DP_ASSERT( pgsit != es->endParameterGroupSpecs() );
    material->materialParameters = dp::sg::core::ParameterGroupData::create( *pgsit );

    Vec3f ambientColor = material->ambientIntensity * interpretSFColor( material->diffuseColor );
    Vec3f diffuseColor = interpretSFColor( material->diffuseColor );
    Vec3f specularColor = interpretSFColor( material->specularColor );
    float specularExponent = 128.0f * material->shininess;
    Vec3f emissiveColor = interpretSFColor( material->emissiveColor );
    float opacity = 1.0f - material->transparency;

    DP_VERIFY( material->materialParameters->setParameter( "backAmbientColor", ambientColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "backDiffuseColor", diffuseColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "backSpecularColor", specularColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "backSpecularExponent", specularExponent ) );
    DP_VERIFY( material->materialParameters->setParameter( "backEmissiveColor", emissiveColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "backOpacity", opacity ) );
    DP_VERIFY( material->materialParameters->setParameter( "frontAmbientColor", ambientColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "frontDiffuseColor", diffuseColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "frontSpecularColor", specularColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "frontSpecularExponent", specularExponent ) );
    DP_VERIFY( material->materialParameters->setParameter( "frontEmissiveColor", emissiveColor ) );
    DP_VERIFY( material->materialParameters->setParameter( "frontOpacity", opacity ) );
    DP_VERIFY( material->materialParameters->setParameter( "unlitColor", Vec4f( diffuseColor, opacity ) ) );
  }
  return( material->materialParameters );
}

ParameterGroupDataSharedPtr WRLLoader::interpretMovieTexture( const SmartPtr<MovieTexture> & pMovieTexture )
{
  onUnsupportedToken( "VRMLLoader", "MovieTexture" );
  return( ParameterGroupDataSharedPtr() );
}

void WRLLoader::interpretNormal( const SmartPtr<Normal> & pNormal )
{
  if ( !pNormal->interpreted )
  {
    if ( pNormal->set_vector )
    {
      interpretNormalInterpolator( pNormal->set_vector.get(), checked_cast<unsigned int>(pNormal->vector.size()) );
    }
    pNormal->interpreted = true;
  }
}

void WRLLoader::interpretNormalInterpolator( const SmartPtr<NormalInterpolator> & pNormalInterpolator
                                           , unsigned int vectorCount )
{
  if ( ! pNormalInterpolator->interpreted )
  {
    SFTime cycleInterval = pNormalInterpolator->set_fraction ? pNormalInterpolator->set_fraction->cycleInterval : 1.0;
    resampleKeyValues( pNormalInterpolator->key, pNormalInterpolator->keyValue, vectorCount
                     , pNormalInterpolator->steps, cycleInterval );
    for_each( pNormalInterpolator->keyValue.begin(), pNormalInterpolator->keyValue.end()
            , std::mem_fun_ref(&Vecnt<3,float>::normalize) );
    pNormalInterpolator->interpreted = true;
  }
}

void  WRLLoader::interpretOrientationInterpolator( const SmartPtr<OrientationInterpolator> & pOrientationInterpolator )
{
  if ( ! pOrientationInterpolator->interpreted )
  {
    SFTime cycleInterval = pOrientationInterpolator->set_fraction
                           ? pOrientationInterpolator->set_fraction->cycleInterval
                           : 1.0;
    resampleKeyValues( pOrientationInterpolator->key, pOrientationInterpolator->keyValue, 1
                     , pOrientationInterpolator->steps, cycleInterval );
    pOrientationInterpolator->keyValueQuatf.resize( pOrientationInterpolator->keyValue.size() );
    for ( size_t i=0 ;i<pOrientationInterpolator->keyValueQuatf.size() ; i++ )
    {
      pOrientationInterpolator->keyValueQuatf[i] = interpretSFRotation( pOrientationInterpolator->keyValue[i] );
    }
    pOrientationInterpolator->interpreted = true;
  }
}

ParameterGroupDataSharedPtr WRLLoader::interpretPixelTexture( const SmartPtr<PixelTexture> & pPixelTexture )
{
  onUnsupportedToken( "VRMLLoader", "PixelTexture" );
  return( ParameterGroupDataSharedPtr() );
}

LightSourceSharedPtr WRLLoader::interpretPointLight( const SmartPtr<vrml::PointLight> & pVRMLPointLight )
{
  LightSourceSharedPtr lightSource;
  if ( pVRMLPointLight->lightSource )
  {
    lightSource = pVRMLPointLight->lightSource;
  }
  else
  {
    Vec3f color( interpretSFColor( pVRMLPointLight->color ) );
    lightSource = createStandardPointLight( pVRMLPointLight->location
                                          , pVRMLPointLight->ambientIntensity * color
                                          , pVRMLPointLight->intensity * color
                                          , pVRMLPointLight->intensity * color
                                          , makeArray( pVRMLPointLight->attenuation[0], pVRMLPointLight->attenuation[1], pVRMLPointLight->attenuation[2] ) );
    lightSource->setName( pVRMLPointLight->getName() );
    lightSource->setEnabled( pVRMLPointLight->on );
  }
  return( lightSource );
}

void  WRLLoader::interpretPointSet( const SmartPtr<PointSet> & pPointSet, vector<PrimitiveSharedPtr> &primitives )
{
  DP_ASSERT( pPointSet->coord );

  if ( pPointSet->pPoints )
  {
    primitives.push_back( pPointSet->pPoints );
  }
  else
  {
    VertexAttributeSetSharedPtr cvas = VertexAttributeSet::create();

    DP_ASSERT( dynamic_cast<const Coordinate *>(pPointSet->coord.get()) );
    const Coordinate * pCoordinate = static_cast<const Coordinate *>(pPointSet->coord.get());
    DP_ASSERT( pCoordinate->point.size() < UINT_MAX );
    vector<Vec3f> vertices( pCoordinate->point.size() );
    for ( unsigned i=0 ; i<pCoordinate->point.size() ; i++ )
    {
      vertices[i] = pCoordinate->point[i];
    }
    cvas->setVertices( &vertices[0], checked_cast<unsigned int>(vertices.size()) );

    if ( pPointSet->color )
    {
      DP_ASSERT( dynamic_cast<const Color *>(pPointSet->color.get()) );
      const Color * pColor = static_cast<const Color *>(pPointSet->color.get());
      DP_ASSERT( pCoordinate->point.size() <= pColor->color.size() );
      vector<Vec3f> colors( pCoordinate->point.size() );
      for ( size_t i=0 ; i<pCoordinate->point.size() ; i++ )
      {
        colors[i] = pColor->color[i];
      }
      cvas->setColors( &colors[0], checked_cast<unsigned int>(colors.size()) );
    }

    PrimitiveSharedPtr pPoints = Primitive::create( PRIMITIVE_POINTS );
    pPoints->setName( pPointSet->getName() );
    pPoints->setVertexAttributeSet( cvas );

    primitives.push_back( pPoints );
    pPointSet->pPoints = pPoints;
  }
}

void WRLLoader::interpretPositionInterpolator( const SmartPtr<PositionInterpolator> & pPositionInterpolator )
{
  if ( !pPositionInterpolator->interpreted )
  {
    SFTime cycleInterval = pPositionInterpolator->set_fraction ? pPositionInterpolator->set_fraction->cycleInterval : 1.0;
    resampleKeyValues( pPositionInterpolator->key, pPositionInterpolator->keyValue, 1
                     , pPositionInterpolator->steps, cycleInterval );
    pPositionInterpolator->interpreted = true;
  }
}

ObjectSharedPtr WRLLoader::interpretSFNode( const SFNode n )
{
  ObjectSharedPtr pObject;
  if ( isSmartPtrOf<Appearance>(n) )
  {
    DP_ASSERT( false );
  }
  else if ( isSmartPtrOf<Background>(n) )
  {
    interpretBackground( smart_cast<Background>(n) );
  }
  else if ( isSmartPtrOf<Coordinate>(n) )
  {
    //  NOP
  }
  else if ( isSmartPtrOf<vrml::Group>(n) )
  {
    if ( isSmartPtrOf<vrml::Billboard>(n) )
    {
      pObject = interpretBillboard( smart_cast<vrml::Billboard>(n) );
    }
    else if ( isSmartPtrOf<vrml::LOD>(n) )
    {
      pObject = interpretLOD( smart_cast<vrml::LOD>(n) );
    }
    else if ( isSmartPtrOf<vrml::Switch>(n) )
    {
      pObject = interpretSwitch( smart_cast<vrml::Switch>(n) );
    }
    else if ( isSmartPtrOf<vrml::Transform>(n) )
    {
      pObject = interpretTransform( smart_cast<vrml::Transform>(n) );
    }
    else
    {
      DP_ASSERT( isSmartPtrOf<vrml::Group>(n) );
      pObject = interpretGroup( smart_cast<vrml::Group>(n) );
    }
  }
  else if ( isSmartPtrOf<Inline>(n) )
  {
    pObject = interpretInline( smart_cast<Inline>(n) );
  }
  else if ( isSmartPtrOf<Interpolator>(n) )
  {
    //  NOP
  }
  else if ( isSmartPtrOf<Light>(n) )
  {
    if ( isSmartPtrOf<DirectionalLight>(n) )
    {
      pObject = interpretDirectionalLight( smart_cast<DirectionalLight>(n) );
    }
    else if ( isSmartPtrOf<vrml::PointLight>(n) )
    {
      pObject = interpretPointLight( smart_cast<vrml::PointLight>(n) );
    }
    else
    {
      DP_ASSERT( isSmartPtrOf<vrml::SpotLight>(n) );
      pObject = interpretSpotLight( smart_cast<vrml::SpotLight>(n) );
    }
  }
  else if ( isSmartPtrOf<vrml::Shape>(n) )
  {
    pObject = interpretShape( smart_cast<vrml::Shape>(n) );
  }
  else if ( isSmartPtrOf<TimeSensor>(n) )
  {
    //  NOP
  }
  else if ( isSmartPtrOf<Viewpoint>(n) )
  {
    pObject = interpretViewpoint( smart_cast<Viewpoint>(n) );
  }
  else
  {
    DP_ASSERT( false );
  }

  return( pObject );
}

Quatf WRLLoader::interpretSFRotation( const SFRotation &r )
{
  Vec3f v( r[0], r[1], r[2] );
  v.normalize();
  return( Quatf( v, r[3] ) );
}

NodeSharedPtr WRLLoader::interpretShape( const SmartPtr<vrml::Shape> & pShape )
{
  if ( ! pShape->pNode )
  {
    EffectDataSharedPtr materialEffect;
    vector<PrimitiveSharedPtr> primitives;

    if ( pShape->appearance )
    {
      DP_ASSERT( isSmartPtrOf<Appearance>(pShape->appearance) );
      materialEffect = interpretAppearance( smart_cast<Appearance>(pShape->appearance) );
    }
    if ( pShape->geometry )
    {
      DP_ASSERT(    isSmartPtrOf<Geometry>(pShape->geometry)
                  &&  ( !pShape->appearance || isSmartPtrOf<Appearance>(pShape->appearance) ) );
      interpretGeometry( smart_cast<Geometry>(pShape->geometry), primitives
                       , pShape->appearance && smart_cast<Appearance>(pShape->appearance)->texture );
    }

    if ( materialEffect && pShape->geometry )
    {
      const ParameterGroupDataSharedPtr & pgd = materialEffect->findParameterGroupData( string( "standardTextureParameters" ) );
      if ( pgd )
      {
        if ( isSmartPtrOf<IndexedFaceSet>(pShape->geometry) )
        {
          const SmartPtr<IndexedFaceSet> & pIndexedFaceSet = smart_cast<IndexedFaceSet>(pShape->geometry);
          if ( ! pIndexedFaceSet->texCoord )
          {
            determineTexGen( pIndexedFaceSet, pgd );
          }
        }
        else if (   isSmartPtrOf<Box>(pShape->geometry)
                ||  isSmartPtrOf<Cone>(pShape->geometry)
                ||  isSmartPtrOf<Cylinder>(pShape->geometry)
                ||  isSmartPtrOf<ElevationGrid>(pShape->geometry)
                ||  isSmartPtrOf<IndexedLineSet>(pShape->geometry)
                ||  isSmartPtrOf<PointSet>(pShape->geometry)
                ||  isSmartPtrOf<Sphere>(pShape->geometry) )
        {
          //  NOP
        }
        else if (   isSmartPtrOf<Extrusion>(pShape->geometry)
                ||  isSmartPtrOf<Text>(pShape->geometry) )
        {
          DP_ASSERT( false );
        }
      }
    }

    if ( ! primitives.empty() )
    {
      if ( primitives.size() == 1 )
      {
        GeoNodeSharedPtr geoNode = GeoNode::create();
        geoNode->setName( pShape->getName() );
        geoNode->setMaterialEffect( materialEffect );
        geoNode->setPrimitive( primitives[0] );
        pShape->pNode = geoNode;
      }
      else
      {
        GroupSharedPtr group = dp::sg::core::Group::create();
        group->setName( pShape->getName() );
        for ( size_t i=0 ; i<primitives.size(); i++ )
        {
          GeoNodeSharedPtr geoNode = GeoNode::create();
          geoNode->setMaterialEffect( materialEffect );
          geoNode->setPrimitive( primitives[i] );
          group->addChild( geoNode );
        }
        pShape->pNode = group;
      }
    }
  }
  return( pShape->pNode );
}

LightSourceSharedPtr WRLLoader::interpretSpotLight( const SmartPtr<vrml::SpotLight> & pVRMLSpotLight )
{
  LightSourceSharedPtr lightSource;
  if ( pVRMLSpotLight->lightSource )
  {
    lightSource = pVRMLSpotLight->lightSource;
  }
  else
  {
    Vec3f color( interpretSFColor( pVRMLSpotLight->color ) );
    float exponent = FLT_EPSILON < pVRMLSpotLight->beamWidth
                    ? pVRMLSpotLight->beamWidth < pVRMLSpotLight->cutOffAngle
                      ? pVRMLSpotLight->cutOffAngle / pVRMLSpotLight->beamWidth
                      : 0.0f
                    : 10000.0f;   // some really large value for beamWidth == 0
    lightSource = createStandardSpotLight( pVRMLSpotLight->location
                                         , pVRMLSpotLight->direction
                                         , pVRMLSpotLight->ambientIntensity * color
                                         , pVRMLSpotLight->intensity * color
                                         , pVRMLSpotLight->intensity * color
                                         , makeArray( pVRMLSpotLight->attenuation[0], pVRMLSpotLight->attenuation[1], pVRMLSpotLight->attenuation[2] )
                                         , exponent
                                         , pVRMLSpotLight->cutOffAngle );
    lightSource->setName( pVRMLSpotLight->getName() );
    lightSource->setEnabled( pVRMLSpotLight->on );
  }
  return( lightSource );
}

ParameterGroupDataSharedPtr WRLLoader::interpretTexture( const SmartPtr<vrml::Texture> & pTexture )
{
  ParameterGroupDataSharedPtr textureData;
  if ( isSmartPtrOf<ImageTexture>(pTexture) )
  {
    textureData = interpretImageTexture( smart_cast<ImageTexture>(pTexture) );
  }
  else if ( isSmartPtrOf<MovieTexture>(pTexture) )
  {
    textureData = interpretMovieTexture( smart_cast<MovieTexture>(pTexture) );
  }
  else
  {
    DP_ASSERT( isSmartPtrOf<PixelTexture>(pTexture) );
    textureData = interpretPixelTexture( smart_cast<PixelTexture>(pTexture) );
  }
  return( textureData );
}

SwitchSharedPtr WRLLoader::interpretSwitch( const SmartPtr<vrml::Switch> & pVRMLSwitch )
{
  SwitchSharedPtr pNVSGSwitch;

  if ( pVRMLSwitch->pSwitch )
  {
    pNVSGSwitch = pVRMLSwitch->pSwitch;
  }
  else
  {
    pNVSGSwitch = dp::sg::core::Switch::create();
    pNVSGSwitch->setName( pVRMLSwitch->getName() );
    interpretChildren( pVRMLSwitch->children, pNVSGSwitch );
    if ( ( pVRMLSwitch->whichChoice < 0 ) || ( (SFInt32)pVRMLSwitch->children.size() <= pVRMLSwitch->whichChoice ) )
    {
      pNVSGSwitch->setInactive();
    }
    else
    {
      pNVSGSwitch->setActive( pVRMLSwitch->whichChoice );
    }
    pVRMLSwitch->pSwitch = pNVSGSwitch;
  }

  return( pNVSGSwitch );
}

void WRLLoader::interpretTextureTransform( const SmartPtr<TextureTransform> & pTextureTransform
                                         , const ParameterGroupDataSharedPtr & textureData )
{
  Trafo t;
  t.setCenter( Vec3f( pTextureTransform->center[0], pTextureTransform->center[1], 0.0f ) );
  t.setOrientation( Quatf( Vec3f( 0.0f, 0.0f, 1.0f ), pTextureTransform->rotation) );
  t.setScaling( Vec3f( pTextureTransform->scale[0], pTextureTransform->scale[1], 1.0f ) );
  t.setTranslation( Vec3f( pTextureTransform->translation[0], pTextureTransform->translation[1], 0.0f ) );

  DP_VERIFY( textureData->setParameter<Mat44f>( "textureMatrix", t.getMatrix() ) );
}

TransformSharedPtr WRLLoader::interpretTransform( const SmartPtr<vrml::Transform> & pVRMLTransform )
{
  TransformSharedPtr pNVSGTransform;

  if ( pVRMLTransform->pTransform )
  {
    pNVSGTransform = pVRMLTransform->pTransform;
  }
  else
  {
#if defined(KEEP_ANIMATION)
    const SmartPtr<PositionInterpolator> & center = pVRMLTransform->set_center;
    const SmartPtr<OrientationInterpolator> & rotation = pVRMLTransform->set_rotation;
    const SmartPtr<PositionInterpolator> & scale = pVRMLTransform->set_scale;
    const SmartPtr<PositionInterpolator> & translation = pVRMLTransform->set_translation;
    if ( center || rotation || scale || translation )
    {
      if (center )
      {
        interpretPositionInterpolator( center );
      }
      Quatf transformRot;
      if ( rotation )
      {
        interpretOrientationInterpolator( rotation );
      }
      else
      {
        transformRot = interpretSFRotation( pVRMLTransform->rotation );
      }
      if ( scale )
      {
        interpretPositionInterpolator( scale );
      }
      if ( translation )
      {
        interpretPositionInterpolator( translation );
      }
      Quatf scaleOrientation = interpretSFRotation( pVRMLTransform->scaleOrientation );

      vector<unsigned int> keys = getCombinedKeys( center, rotation, scale, translation );

      LinearInterpolatedTrafoAnimationDescriptionSharedPtr litadh = LinearInterpolatedTrafoAnimationDescription::create();
      LinearInterpolatedTrafoAnimationDescriptionLock liadt( litadh );
      liadt->reserveKeys( checked_cast<unsigned int>(keys.size()) );

      for ( size_t i=0, centerStep=0, rotationStep=0, scaleStep=0, translationStep=0 ; i<keys.size() ; i++ )
      {
        Trafo trafo;
        if ( center )
        {
          if ( center->steps[centerStep] < keys[i] )
          {
            centerStep++;
            DP_ASSERT( keys[i] <= center->steps[centerStep] );
          }
          if ( keys[i] == center->steps[centerStep] )
          {
            trafo.setCenter( center->keyValue[centerStep] );
          }
          else
          {
            DP_ASSERT( keys[i] < center->steps[centerStep] );
            float alpha = (float)( keys[i] - center->steps[centerStep-1] )
                        / (float)( center->steps[centerStep] - center->steps[centerStep-1] );
            trafo.setCenter( lerp( alpha, center->keyValue[centerStep-1], center->keyValue[centerStep] ) );
          }
        }
        else
        {
          trafo.setCenter( pVRMLTransform->center );
        }
        if ( rotation )
        {
          if ( rotation->steps[rotationStep] < keys[i] )
          {
            rotationStep++;
            DP_ASSERT( keys[i] <= rotation->steps[rotationStep] );
          }
          if ( keys[i] == rotation->steps[rotationStep] )
          {
            trafo.setOrientation( rotation->keyValueQuatf[rotationStep] );
          }
          else
          {
            DP_ASSERT( keys[i] < rotation->steps[rotationStep] );
            float alpha = (float)( keys[i] - rotation->steps[rotationStep-1] )
                        / (float)( rotation->steps[rotationStep] - rotation->steps[rotationStep-1] );
            Quatf rot = lerp( alpha, rotation->keyValueQuatf[rotationStep-1], rotation->keyValueQuatf[rotationStep] );
            rot.normalize();
            trafo.setOrientation( rot );
          }
        }
        else
        {
          trafo.setOrientation( transformRot );
        }
        if ( scale )
        {
          if ( scale->steps[scaleStep] < keys[i] )
          {
            scaleStep++;
            DP_ASSERT( keys[i] <= scale->steps[scaleStep] );
          }
          if ( keys[i] == scale->steps[scaleStep] )
          {
            trafo.setScaling( scale->keyValue[scaleStep] );
          }
          else
          {
            DP_ASSERT( keys[i] < scale->steps[scaleStep] );
            float alpha = (float)( keys[i] - scale->steps[scaleStep-1] )
                        / (float)( scale->steps[scaleStep] - scale->steps[scaleStep-1] );
            trafo.setScaling( lerp( alpha, scale->keyValue[scaleStep-1], scale->keyValue[scaleStep] ) );
          }
        }
        else
        {
          trafo.setScaling( pVRMLTransform->scale );
        }
        trafo.setScaleOrientation( scaleOrientation );
        if ( translation )
        {
          if ( translation->steps[translationStep] < keys[i] )
          {
            translationStep++;
            DP_ASSERT( keys[i] <= translation->steps[translationStep] );
          }
          if ( keys[i] == translation->steps[translationStep] )
          {
            trafo.setTranslation( translation->keyValue[translationStep] );
          }
          else
          {
            DP_ASSERT( keys[i] < translation->steps[translationStep] );
            float alpha = (float)( keys[i] - translation->steps[translationStep-1] )
                        / (float)( translation->steps[translationStep] - translation->steps[translationStep-1] );
            trafo.setTranslation( lerp( alpha, translation->keyValue[translationStep-1], translation->keyValue[translationStep] ) );
          }
        }
        else
        {
          trafo.setTranslation( pVRMLTransform->translation );
        }
        liadt->addKey( keys[i], trafo );
      }

      TrafoAnimationSharedPtr tah = TrafoAnimation::create();
      TrafoAnimationLock(tah)->setDescription( litadh );

      AnimatedTransformSharedPtr pNVSGAnimatedTransform = AnimatedTransform::create();
      AnimatedTransformLock(pNVSGAnimatedTransform)->setAnimation(tah);
      pNVSGTransform = pNVSGAnimatedTransform;
    }
    else
#endif
    {
      Trafo trafo;
      trafo.setCenter( pVRMLTransform->center );
      trafo.setOrientation( interpretSFRotation( pVRMLTransform->rotation ) );
      trafo.setScaling( pVRMLTransform->scale );
      trafo.setScaleOrientation( interpretSFRotation( pVRMLTransform->scaleOrientation ) );
      trafo.setTranslation( pVRMLTransform->translation );

      pNVSGTransform = dp::sg::core::Transform::create();
      pNVSGTransform->setTrafo( trafo );
    }

    pNVSGTransform->setName( pVRMLTransform->getName() );
    interpretChildren( pVRMLTransform->children, pNVSGTransform );

    pVRMLTransform->pTransform = pNVSGTransform;
  }

  return( pNVSGTransform );
}

bool  WRLLoader::interpretURL( const MFString &url, string &fileName )
{
  bool found = false;
  for ( size_t i=0 ; ! found && i<url.size() ; i++ )
  {
    fileName = dp::util::findFile( url[i], m_searchPaths );
    found = !fileName.empty();
  }
  onFilesNotFound( found, url );
  return( found );
}

ObjectSharedPtr  WRLLoader::interpretViewpoint( const SmartPtr<Viewpoint> & pViewpoint )
{
#if (KEEP_ANIMATION)
  const SmartPtr<OrientationInterpolator> & orientation = pViewpoint->set_orientation;
  const SmartPtr<PositionInterpolator> & position = pViewpoint->set_position;
  if ( orientation || position )
  {
    Quatf rot;
    if ( orientation )
    {
      interpretOrientationInterpolator( orientation );
    }
    else
    {
      rot = interpretSFRotation( pViewpoint->orientation );
    }
    if ( position )
    {
      interpretPositionInterpolator( position );
    }

    vector<unsigned int> keys = getCombinedKeys( position, orientation, NULL, NULL );
    LinearInterpolatedTrafoAnimationDescriptionSharedPtr litadh = LinearInterpolatedTrafoAnimationDescription::create();
    {
      LinearInterpolatedTrafoAnimationDescriptionLock liadt(litadh);
      liadt->reserveKeys( checked_cast<unsigned int>(keys.size()) );

      for ( unsigned int i=0, orientationStep=0, positionStep=0 ; i<keys.size() ; i++ )
      {
        Trafo trafo;
        if ( orientation )
        {
          if ( orientation->steps[orientationStep] < keys[i] )
          {
            orientationStep++;
            DP_ASSERT( keys[i] <= orientation->steps[orientationStep] );
          }
          if ( keys[i] == orientation->steps[orientationStep] )
          {
            trafo.setOrientation( orientation->keyValueQuatf[orientationStep] );
          }
          else
          {
            DP_ASSERT( keys[i] < orientation->steps[orientationStep] );
            float alpha = (float)( keys[i] - orientation->steps[orientationStep-1] )
                        / (float)( orientation->steps[orientationStep] - orientation->steps[orientationStep-1] );
            Quatf rot = lerp( alpha, orientation->keyValueQuatf[orientationStep-1], orientation->keyValueQuatf[orientationStep] );
            rot.normalize();
            trafo.setOrientation( rot );
          }
        }
        else
        {
          trafo.setOrientation( rot );
        }
        if ( position )
        {
          if ( position->steps[positionStep] < keys[i] )
          {
            positionStep++;
            DP_ASSERT( keys[i] <= position->steps[positionStep] );
          }
          if ( keys[i] == position->steps[positionStep] )
          {
            trafo.setTranslation( position->keyValue[positionStep] );
          }
          else
          {
            DP_ASSERT( keys[i] < position->steps[positionStep] );
            float alpha = (float)( keys[i] - position->steps[positionStep-1] )
                        / (float)( position->steps[positionStep] - position->steps[positionStep-1] );
            trafo.setTranslation( lerp( alpha, position->keyValue[positionStep-1], position->keyValue[positionStep] ) );
          }
        }
        else
        {
          trafo.setTranslation( pViewpoint->position );
        }
        liadt->addKey( keys[i], trafo );
      }
    }

    TrafoAnimationSharedPtr tah = TrafoAnimation::create();
    TrafoAnimationLock(tah)->setDescription( litadh );

    m_scene->addCameraAnimation( tah );
  }
#endif

  PerspectiveCameraSharedPtr pc( PerspectiveCamera::create() );
  pc->setName( pViewpoint->getName() );
  pc->setFieldOfView( pViewpoint->fieldOfView );
  pc->setOrientation( interpretSFRotation( pViewpoint->orientation ) );
  pc->setPosition( pViewpoint->position );

  m_scene->addCamera( pc );

  return( ObjectSharedPtr() );
}

bool  WRLLoader::isValidScaling( const SmartPtr<PositionInterpolator> & pPositionInterpolator ) const
{
  bool  isValid = ! pPositionInterpolator->keyValue.empty();
  for ( size_t i=0 ; isValid && i<pPositionInterpolator->keyValue.size() ; i++ )
  {
    isValid &= isValidScaling( pPositionInterpolator->keyValue[i] );
  }
  return( isValid );
}

bool  WRLLoader::isValidScaling( const SFVec3f &sfVec3f ) const
{
  return( ( FLT_EPSILON < sfVec3f[0] ) && ( FLT_EPSILON < sfVec3f[1] ) && ( FLT_EPSILON < sfVec3f[2] ) );
}

SceneSharedPtr WRLLoader::load( string const& filename, vector<string> const& searchPaths, dp::sg::ui::ViewStateSharedPtr & viewState )
{
  if ( !dp::util::fileExists(filename) )
  {
    throw dp::FileNotFoundException( filename );
  }

  DP_ASSERT( m_textureFiles.empty() );

  // set the locale temporarily to the default "C" to make atof behave predictably
  dp::util::TempLocale tl("C"); 

  //  (re-)initialize member variables
  m_eof = false;
  m_smoothTraverser = std::make_shared<dp::sg::algorithm::SmoothTraverser>();

  viewState.reset(); // loading of ViewState currently not supported

  // private copy of the search paths
  m_searchPaths = searchPaths;

  //  add the path to the found file to the search paths
  string dir = dp::util::getFilePath( filename );
  if ( std::find( m_searchPaths.begin(), m_searchPaths.end(), dir ) == m_searchPaths.end() )
  {
    m_searchPaths.push_back( dir );
  }

  // run the importer
  try
  {
    m_scene = import( filename );
  }
  catch( ... )
  {
    if ( m_fh )
    {
      fclose( m_fh );
      m_fh = NULL;
    }

    //clean up resources
    m_textureFiles.clear();
    m_smoothTraverser = nullptr;
    m_rootNode.reset();
    m_scene.reset();

    throw;
  }
  if ( ! m_scene && callback() )
  {
    callback()->onFileEmpty( filename );
  }

  SceneSharedPtr scene = m_scene;

  //clean up resources
  m_textureFiles.clear();
  m_smoothTraverser = nullptr;
  m_rootNode.reset();
  m_scene.reset();

  if ( !scene )
  {
    throw std::runtime_error( std::string("Empty scene loaded") );
  }
  return( scene );
}

bool  WRLLoader::onIncompatibleValues( int value0, int value1, const string &node, const string &field0, const string &field1 ) const
{
  return( callback() ? callback()->onIncompatibleValues( m_lineNumber, node, field0, value0, field1, value1) : true );
}

template<typename T> bool  WRLLoader::onInvalidValue( T value, const string &node, const string &field ) const
{
  return( callback() ? callback()->onInvalidValue( m_lineNumber, node, field, value ) : true );
}

bool  WRLLoader::onEmptyToken( const string &tokenType, const string &token ) const
{
  return( callback() ? callback()->onEmptyToken( m_lineNumber, tokenType, token ) : true );
}

bool  WRLLoader::onFileNotFound( const SFString &url ) const
{
  return( callback() ? callback()->onFileNotFound( url ) : true );
}

bool  WRLLoader::onFilesNotFound( bool found, const MFString &url ) const
{
  if ( !found && callback() )
  {
    return( callback()->onFilesNotFound( url ) );
  }
  return( true );
}

void  WRLLoader::onUnexpectedEndOfFile( bool error ) const
{
  if ( callback() && error )
  {
    callback()->onUnexpectedEndOfFile( m_lineNumber );
  }
}

void  WRLLoader::onUnexpectedToken( const string &expected, const string &token ) const
{
  if ( expected != token && callback() )
  {
    callback()->onUnexpectedToken( m_lineNumber, expected, token );
  }
}

void  WRLLoader::onUnknownToken( const string &tokenType, const string &token ) const
{
  if ( callback() )
  {
    callback()->onUnknownToken( m_lineNumber, tokenType, token );
  }
}

bool  WRLLoader::onUndefinedToken( const string &tokenType, const string &token ) const
{
  return( callback() ? callback()->onUndefinedToken( m_lineNumber, tokenType, token ) : true );
}

bool  WRLLoader::onUnsupportedToken( const string &tokenType, const string &token ) const
{
  return( callback() ? callback()->onUnsupportedToken( m_lineNumber, tokenType, token ) : true );
}

SmartPtr<Anchor> WRLLoader::readAnchor( const string &nodeName )
{
  SmartPtr<Anchor> pAnchor( new Anchor );
  pAnchor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "children" )
    {
      readMFNode( pAnchor );
    }
    else if ( token == "description" )
    {
      readSFString( pAnchor->description, getNextToken() );
    }
    else if ( token == "parameter" )
    {
      readMFString( pAnchor->parameter );
    }
    else if ( token == "url" )
    {
      readMFString( pAnchor->url );
    }
    else
    {
      onUnknownToken( "Anchor", token );
    }
    token = getNextToken();
  }

  return( pAnchor );
}

SmartPtr<Appearance> WRLLoader::readAppearance( const string &nodeName )
{
  SmartPtr<Appearance> pAppearance( new Appearance );
  pAppearance->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "material" )
    {
      readSFNode( pAppearance, pAppearance->material, getNextToken() );
      if ( pAppearance->material && ! isSmartPtrOf<vrml::Material>( pAppearance->material ) )
      {
        onUnsupportedToken( "Appearance.material", pAppearance->material->getType() );
        pAppearance->material.reset();
      }
    }
    else if ( token == "texture" )
    {
      readSFNode( pAppearance, pAppearance->texture, getNextToken() );
      if ( pAppearance->texture && ! isSmartPtrOf<vrml::Texture>( pAppearance->texture ) )
      {
        onUnsupportedToken( "Appearance.texture", pAppearance->texture->getType() );
        pAppearance->texture.reset();
      }
    }
    else if ( token == "textureTransform" )
    {
      readSFNode( pAppearance, pAppearance->textureTransform, getNextToken() );
      if ( pAppearance->textureTransform && ! isSmartPtrOf<TextureTransform>( pAppearance->textureTransform ) )
      {
        onUnsupportedToken( "Appearance.textureTransform", pAppearance->textureTransform->getType() );
        pAppearance->textureTransform.reset();
      }
    }
    else
    {
      onUnknownToken( "Appearance", token );
    }
    token = getNextToken();
  }

  return( pAppearance );
}

SmartPtr<AudioClip> WRLLoader::readAudioClip( const string &nodeName )
{
  SmartPtr<AudioClip> pAudioClip( new AudioClip );
  pAudioClip->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "description" )
    {
      readSFString( pAudioClip->description, getNextToken() );
    }
    else if ( token == "loop" )
    {
      readSFBool( pAudioClip->loop );
    }
    else if ( token == "pitch" )
    {
      readSFFloat( pAudioClip->pitch, getNextToken() );
    }
    else if ( token == "startTime" )
    {
      readSFTime( pAudioClip->startTime );
    }
    else if ( token == "stopTime" )
    {
      readSFTime( pAudioClip->stopTime );
    }
    else if ( token == "url" )
    {
      readMFString( pAudioClip->url );
    }
    else
    {
      onUnknownToken( "AudioClip", token );
    }
    token = getNextToken();
  }

  return( pAudioClip );
}

SmartPtr<Background> WRLLoader::readBackground( const string &nodeName )
{
  SmartPtr<Background> pBackground( new Background );
  pBackground->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "groundAngle" )
    {
      readMFFloat( pBackground->groundAngle );
    }
    else if ( token == "groundColor" )
    {
      readMFColor( pBackground->groundColor );
    }
    else if ( token == "backUrl" )
    {
      readMFString( pBackground->backUrl );
    }
    else if ( token == "bottomUrl" )
    {
      readMFString( pBackground->bottomUrl );
    }
    else if ( token == "frontUrl" )
    {
      readMFString( pBackground->frontUrl );
    }
    else if ( token == "leftUrl" )
    {
      readMFString( pBackground->leftUrl );
    }
    else if ( token == "rightUrl" )
    {
      readMFString( pBackground->rightUrl );
    }
    else if ( token == "topUrl" )
    {
      readMFString( pBackground->topUrl );
    }
    else if ( token == "skyAngle" )
    {
      readMFFloat( pBackground->skyAngle );
    }
    else if ( token == "skyColor" )
    {
      pBackground->skyColor.clear();
      readMFColor( pBackground->skyColor );
    }
    else
    {
      onUnknownToken( "Background", token );
    }
    token = getNextToken();
  }

  return( pBackground );
}

SmartPtr<vrml::Billboard> WRLLoader::readBillboard( const string &nodeName )
{
  SmartPtr<vrml::Billboard> pBillboard( new vrml::Billboard );
  pBillboard->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "axisOfRotation" )
    {
      readSFVec3f( pBillboard->axisOfRotation, getNextToken() );
    }
    else if ( token == "children" )
    {
      readMFNode( pBillboard );
    }
    else
    {
      onUnknownToken( "Billboard", token );
    }
    token = getNextToken();
  }

  return( pBillboard );
}

SmartPtr<Box> WRLLoader::readBox( const string &nodeName )
{
  SmartPtr<Box> pBox( new Box );
  pBox->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "size" )
    {
      readSFVec3f( pBox->size, getNextToken() );
    }
    else
    {
      onUnknownToken( "Box", token );
    }
    token = getNextToken();
  }

  return( pBox );
}

SmartPtr<Collision> WRLLoader::readCollision( const string &nodeName )
{
  SmartPtr<Collision> pCollision( new Collision );
  pCollision->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "children" )
    {
      readMFNode( pCollision );
    }
    else if ( token == "collide" )
    {
      readSFBool( pCollision->collide );
    }
    else if ( token == "proxy" )
    {
      readSFNode( pCollision, pCollision->proxy, getNextToken() );
    }
    else
    {
      onUnknownToken( "Collision", token );
    }
    token = getNextToken();
  }

  return( pCollision );
}

SmartPtr<Color> WRLLoader::readColor( const string &nodeName )
{
  SmartPtr<Color> pColor( new Color );
  pColor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "color" )
    {
      readMFColor( pColor->color );
    }
    else
    {
      onUnknownToken( "Color", token );
    }
    token = getNextToken();
  }

  return( pColor );
}

SmartPtr<ColorInterpolator> WRLLoader::readColorInterpolator( const string &nodeName )
{
  SmartPtr<ColorInterpolator> pColorInterpolator( new ColorInterpolator );
  pColorInterpolator->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "key" )
    {
      readMFFloat( pColorInterpolator->key );
    }
    else if ( token == "keyValue" )
    {
      readMFColor( pColorInterpolator->keyValue );
    }
    else
    {
      onUnknownToken( "ColorInterpolator", token );
    }
    token = getNextToken();
  }

  DP_ASSERT( ( pColorInterpolator->keyValue.size() % pColorInterpolator->key.size() ) == 0 );

  return( pColorInterpolator );
}

SmartPtr<Cone> WRLLoader::readCone( const string &nodeName )
{
  SmartPtr<Cone> pCone = new Cone;
  pCone->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "bottom" )
    {
      readSFBool( pCone->bottom );
    }
    else if ( token == "bottomRadius" )
    {
      readSFFloat( pCone->bottomRadius, getNextToken() );
    }
    else if ( token == "height" )
    {
      readSFFloat( pCone->height, getNextToken() );
    }
    else if ( token == "side" )
    {
      readSFBool( pCone->side );
    }
    else
    {
      onUnknownToken( "Cone", token );
    }
    token = getNextToken();
  }

  return( pCone );
}

SmartPtr<Coordinate> WRLLoader::readCoordinate( const string &nodeName )
{
  SmartPtr<Coordinate> pCoordinate( new Coordinate );
  pCoordinate->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "point" )
    {
      readMFVec3f( pCoordinate->point );
    }
    else
    {
      onUnknownToken( "Coordinate", token );
    }
    token = getNextToken();
  }

  return( pCoordinate );
}

SmartPtr<CoordinateInterpolator> WRLLoader::readCoordinateInterpolator( const string &nodeName )
{
  SmartPtr<CoordinateInterpolator> pCoordinateInterpolator( new CoordinateInterpolator );
  pCoordinateInterpolator->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "key" )
    {
      readMFFloat( pCoordinateInterpolator->key );
    }
    else if ( token == "keyValue" )
    {
      readMFVec3f( pCoordinateInterpolator->keyValue );
    }
    else
    {
      onUnknownToken( "CoordinateInterpolator", token );
    }
    token = getNextToken();
  }

  DP_ASSERT( ( pCoordinateInterpolator->keyValue.size() % pCoordinateInterpolator->key.size() ) == 0 );

  return( pCoordinateInterpolator );
}

SmartPtr<Cylinder> WRLLoader::readCylinder( const string &nodeName )
{
  SmartPtr<Cylinder> pCylinder = new Cylinder;
  pCylinder->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "bottom" )
    {
      readSFBool( pCylinder->bottom );
    }
    else if ( token == "height" )
    {
      readSFFloat( pCylinder->height, getNextToken() );
    }
    else if ( token == "radius" )
    {
      readSFFloat( pCylinder->radius, getNextToken() );
    }
    else if ( token == "side" )
    {
      readSFBool( pCylinder->side );
    }
    else if ( token == "top" )
    {
      readSFBool( pCylinder->top );
    }
    else
    {
      onUnknownToken( "Cylinder", token );
    }
    token = getNextToken();
  }

  return( pCylinder );
}

SmartPtr<CylinderSensor> WRLLoader::readCylinderSensor( const string &nodeName )
{
  SmartPtr<CylinderSensor> pCylinderSensor = new CylinderSensor;
  pCylinderSensor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "autoOffset" )
    {
      readSFBool( pCylinderSensor->autoOffset );
    }
    else if ( token == "diskAngle" )
    {
      readSFFloat( pCylinderSensor->diskAngle, getNextToken() );
    }
    else if ( token == "enabled" )
    {
      readSFBool( pCylinderSensor->enabled );
    }
    else if ( token == "maxAngle" )
    {
      readSFFloat( pCylinderSensor->maxAngle, getNextToken() );
    }
    else if ( token == "minAngle" )
    {
      readSFFloat( pCylinderSensor->minAngle, getNextToken() );
    }
    else if ( token == "offset" )
    {
      readSFFloat( pCylinderSensor->offset, getNextToken() );
    }
    else
    {
      onUnknownToken( "CylinderSensor", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "CylinderSensor" );
  pCylinderSensor.reset();

  return( pCylinderSensor );
}

SmartPtr<DirectionalLight> WRLLoader::readDirectionalLight( const string &nodeName )
{
  SmartPtr<DirectionalLight> pDirectionalLight( new DirectionalLight );
  pDirectionalLight->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "ambientIntensity" )
    {
      readSFFloat( pDirectionalLight->ambientIntensity, getNextToken() );
    }
    else if ( token == "color" )
    {
      readSFColor( pDirectionalLight->color, getNextToken() );
    }
    else if ( token == "direction" )
    {
      readSFVec3f( pDirectionalLight->direction, getNextToken() );
    }
    else if ( token == "intensity" )
    {
      readSFFloat( pDirectionalLight->intensity, getNextToken() );
    }
    else if ( token == "on" )
    {
      readSFBool( pDirectionalLight->on );
    }
    else
    {
      onUnknownToken( "DirectionalLight", token );
    }
    token = getNextToken();
  }

  return( pDirectionalLight );
}

SmartPtr<ElevationGrid> WRLLoader::readElevationGrid( const string &nodeName )
{
  bool killit = false;

  SmartPtr<ElevationGrid> pElevationGrid = new ElevationGrid;
  pElevationGrid->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "color" )
    {
      readSFNode( pElevationGrid, pElevationGrid->color, getNextToken() );
    }
    else if ( token == "normal" )
    {
      readSFNode( pElevationGrid, pElevationGrid->normal, getNextToken() );
    }
    else if ( token == "texCoord" )
    {
      readSFNode( pElevationGrid, pElevationGrid->texCoord, getNextToken() );
    }
    else if ( token == "height" )
    {
      readMFFloat( pElevationGrid->height );
    }
    else if ( token == "ccw" )
    {
      readSFBool( pElevationGrid->ccw );
    }
    else if ( token == "colorPerVertex" )
    {
      readSFBool( pElevationGrid->colorPerVertex );
    }
    else if ( token == "creaseAngle" )
    {
      readSFFloat( pElevationGrid->creaseAngle, getNextToken() );
    }
    else if ( token == "normalPerVertex" )
    {
      readSFBool( pElevationGrid->normalPerVertex );
    }
    else if ( token == "solid" )
    {
      readSFBool( pElevationGrid->solid );
    }
    else if ( token == "xDimension" )
    {
      readSFInt32( pElevationGrid->xDimension, getNextToken() );
      if ( pElevationGrid->xDimension <= 1 )
      {
        onInvalidValue( pElevationGrid->xDimension, "ElevationGrid", token );
        killit = true;
      }
    }
    else if ( token == "xSpacing" )
    {
      readSFFloat( pElevationGrid->xSpacing, getNextToken() );
      if ( pElevationGrid->xSpacing <= 0.0f )
      {
        onInvalidValue( pElevationGrid->xSpacing, "ElevationGrid", token );
        killit = true;
      }
    }
    else if ( token == "zDimension" )
    {
      readSFInt32( pElevationGrid->zDimension, getNextToken() );
      if ( pElevationGrid->zDimension <= 1 )
      {
        onInvalidValue( pElevationGrid->zDimension, "ElevationGrid", token );
        killit = true;
      }
    }
    else if ( token == "zSpacing" )
    {
      readSFFloat( pElevationGrid->zSpacing, getNextToken() );
      if ( pElevationGrid->zSpacing <= 0.0f )
      {
        onInvalidValue( pElevationGrid->zSpacing, "ElevationGrid", token );
        killit = true;
      }
    }
    else
    {
      onUnknownToken( "ElevationGrid", token );
    }
    token = getNextToken();
  }

  if ( pElevationGrid->height.size() != ( pElevationGrid->xDimension * pElevationGrid->zDimension ) )
  {
    onIncompatibleValues( (int) pElevationGrid->height.size(), (int) pElevationGrid->xDimension * pElevationGrid->zDimension,
                          "ElevationGrid", "height.size", "xDimension * zDimension" );
    killit = true;
  }

  if ( killit )
  {
    pElevationGrid.reset();
  }

  return( pElevationGrid );
}

void  WRLLoader::readEXTERNPROTO( void )
{
  onUnsupportedToken( "VRMLLoader", "EXTERNPROTO" );
  m_PROTONames.insert( getNextToken() );    //  PrototypeName
  ignoreBlock( "[", "]", getNextToken() );  //  PrototypeDeclaration
  MFString  mfString;
  readMFString( mfString );
}

SmartPtr<Extrusion> WRLLoader::readExtrusion( const string &nodeName )
{
  SmartPtr<Extrusion> pExtrusion = new Extrusion;
  pExtrusion->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "beginCap" )
    {
      readSFBool( pExtrusion->beginCap );
    }
    else if ( token == "ccw" )
    {
      readSFBool( pExtrusion->ccw );
    }
    else if ( token == "convex" )
    {
      readSFBool( pExtrusion->convex );
    }
    else if ( token == "creaseAngle" )
    {
      readSFFloat( pExtrusion->creaseAngle, getNextToken() );
    }
    else if ( token == "crossSection" )
    {
      readMFVec2f( pExtrusion->crossSection );
    }
    else if ( token == "endCap" )
    {
      readSFBool( pExtrusion->endCap );
    }
    else if ( token == "orientation" )
    {
      readMFRotation( pExtrusion->orientation );
    }
    else if ( token == "scale" )
    {
      readMFVec2f( pExtrusion->scale );
    }
    else if ( token == "solid" )
    {
      readSFBool( pExtrusion->solid );
    }
    else if ( token == "spine" )
    {
      readMFVec3f( pExtrusion->spine );
    }
    else
    {
      onUnknownToken( "Extrusion", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "Extrusion" );
  pExtrusion.reset();

  return( pExtrusion );
}

SmartPtr<Fog> WRLLoader::readFog( const string &nodeName )
{
  SmartPtr<Fog> pFog = new Fog;
  pFog->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "color" )
    {
      readSFColor( pFog->color, getNextToken() );
    }
    else if ( token == "fogType" )
    {
      readSFString( pFog->fogType, getNextToken() );
    }
    else if ( token == "visibilityRange" )
    {
      readSFFloat( pFog->visibilityRange, getNextToken() );
    }
    else
    {
      onUnknownToken( "Fog", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "Fog" );
  pFog.reset();

  return( pFog );
}

SmartPtr<FontStyle> WRLLoader::readFontStyle( const string &nodeName )
{
  SmartPtr<FontStyle> pFontStyle( new FontStyle );
  pFontStyle->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "family" )
    {
      readMFString( pFontStyle->family );
    }
    else if ( token == "horizontal" )
    {
      readSFBool( pFontStyle->horizontal );
    }
    else if ( token == "justify" )
    {
      readMFString( pFontStyle->justify );
    }
    else if ( token == "language" )
    {
      readSFString( pFontStyle->language, getNextToken() );
    }
    else if ( token == "leftToRight" )
    {
      readSFBool( pFontStyle->leftToRight );
    }
    else if ( token == "size" )
    {
      readSFFloat( pFontStyle->size, getNextToken() );
    }
    else if ( token == "spacing" )
    {
      readSFFloat( pFontStyle->spacing, getNextToken() );
    }
    else if ( token == "style" )
    {
      readSFString( pFontStyle->style, getNextToken() );
    }
    else if ( token == "topToBottom" )
    {
      readSFBool( pFontStyle->topToBottom );
    }
    else
    {
      onUnknownToken( "FontStyle", token );
    }
    token = getNextToken();
  }

  return( pFontStyle );
}

SmartPtr<vrml::Group> WRLLoader::readGroup( const string &nodeName )
{
  SmartPtr<vrml::Group> pGroup( new vrml::Group );
  pGroup->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "bboxCenter" )
    {
      readSFVec3f( pGroup->bboxCenter, getNextToken() );
    }
    else if ( token == "bboxSize" )
    {
      readSFVec3f( pGroup->bboxSize, getNextToken() );
    }
    else if ( token == "children" )
    {
      readMFNode( pGroup );
    }
    else
    {
      onUnknownToken( "Group", token );
    }
    token = getNextToken();
  }

  return( pGroup );
}


SmartPtr<ImageTexture> WRLLoader::readImageTexture( const string &nodeName )
{
  SmartPtr<ImageTexture> pImageTexture( new ImageTexture );
  pImageTexture->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "url" )
    {
      readMFString( pImageTexture->url );
    }
    else if ( token == "repeatS" )
    {
      readSFBool( pImageTexture->repeatS );
    }
    else if ( token == "repeatT" )
    {
      readSFBool( pImageTexture->repeatT );
    }
    else
    {
      onUnknownToken( "ImageTexture", token );
    }
    token = getNextToken();
  }

  return( pImageTexture );
}

void  WRLLoader::readIndex( vector<SFInt32> &mf )
{
  readMFInt32( mf );
  if ( ( mf.size() == 1 ) && ( mf[0] == -1 ) )
  {
    mf.clear();
  }
}

bool removeCollinearPoint( SmartPtr<IndexedFaceSet> & pIndexedFaceSet, unsigned int i0, unsigned int i1, unsigned int i2 )
{
  DP_ASSERT( dynamic_cast<const Coordinate *>(pIndexedFaceSet->coord.get()) );
  const Coordinate * pC = static_cast<const Coordinate *>(pIndexedFaceSet->coord.get());
  Vec3f e0 = pC->point[pIndexedFaceSet->coordIndex[i1]] - pC->point[pIndexedFaceSet->coordIndex[i0]];
  Vec3f e1 = pC->point[pIndexedFaceSet->coordIndex[i2]] - pC->point[pIndexedFaceSet->coordIndex[i1]];
  if ( length( e0 ^ e1 ) <= FLT_EPSILON )
  {
    bool remove = true;
    if ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() )
    {
      DP_ASSERT( dynamic_cast<const Color *>(pIndexedFaceSet->color.get()) );
      const Color * pColor = static_cast<const Color *>(pIndexedFaceSet->color.get());
      Vec3f dc0 = pColor->color[pIndexedFaceSet->colorIndex[i1]] - pColor->color[pIndexedFaceSet->colorIndex[i0]];
      Vec3f dc1 = pColor->color[pIndexedFaceSet->colorIndex[i2]] - pColor->color[pIndexedFaceSet->colorIndex[i1]];
      remove = ( length( dc0 / length( e0 ) - dc1 / length( e1 ) ) < FLT_EPSILON );
    }
    if ( remove && pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() )
    {
      DP_ASSERT( dynamic_cast<const Normal *>(pIndexedFaceSet->normal.get()) );
      const Normal * pNormal = static_cast<const Normal *>(pIndexedFaceSet->normal.get());
      Vec3f nxn0 = pNormal->vector[pIndexedFaceSet->normalIndex[i0]] ^ pNormal->vector[pIndexedFaceSet->normalIndex[i1]];
      Vec3f nxn1 = pNormal->vector[pIndexedFaceSet->normalIndex[i1]] ^ pNormal->vector[pIndexedFaceSet->normalIndex[i2]];
      float c0 = pNormal->vector[pIndexedFaceSet->normalIndex[i0]] * pNormal->vector[pIndexedFaceSet->normalIndex[i1]];
      float c1 = pNormal->vector[pIndexedFaceSet->normalIndex[i1]] * pNormal->vector[pIndexedFaceSet->normalIndex[i2]];
      remove =    areCollinear( nxn0, nxn1 )
              &&  ( abs( acos( c0 ) / length( e0 ) - acos( c1 ) / length( e1 ) ) < FLT_EPSILON );
    }
    if ( remove && pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() )
    {
      DP_ASSERT( dynamic_cast<const TextureCoordinate *>(pIndexedFaceSet->texCoord.get()) );
      const TextureCoordinate * pTextureCoordinate = static_cast<const TextureCoordinate *>(pIndexedFaceSet->texCoord.get());
      Vec2f dt0 = pTextureCoordinate->point[pIndexedFaceSet->texCoordIndex[i1]] - pTextureCoordinate->point[pIndexedFaceSet->texCoordIndex[i0]];
      Vec2f dt1 = pTextureCoordinate->point[pIndexedFaceSet->texCoordIndex[i2]] - pTextureCoordinate->point[pIndexedFaceSet->texCoordIndex[i1]];
      remove = ( length( dt0 / length( e0 ) - dt1 / length( e1 ) ) < FLT_EPSILON );
    }

    // i0-i1-i2 are collinear -> remove i1
    if ( remove )
    {
      pIndexedFaceSet->coordIndex.erase( pIndexedFaceSet->coordIndex.begin() + i1 );
      if ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() )
      {
        pIndexedFaceSet->colorIndex.erase( pIndexedFaceSet->colorIndex.begin() + i1 );
      }
      if ( pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() )
      {
        pIndexedFaceSet->normalIndex.erase( pIndexedFaceSet->normalIndex.begin() + i1 );
      }
      if ( pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() )
      {
        pIndexedFaceSet->texCoordIndex.erase( pIndexedFaceSet->texCoordIndex.begin() + i1 );
      }
      return( true );
    }
  }
  return( false );
}

bool removeRedundantPoint( SmartPtr<IndexedFaceSet> & pIndexedFaceSet, unsigned int i0, unsigned int i1 )
{
  DP_ASSERT( dynamic_cast<const Coordinate *>(pIndexedFaceSet->coord.get()) );
  const Coordinate * pC = static_cast<const Coordinate *>(pIndexedFaceSet->coord.get());
  if (    ( pIndexedFaceSet->coordIndex[i0] == pIndexedFaceSet->coordIndex[i1] )
      ||  ( length( pC->point[pIndexedFaceSet->coordIndex[i1]] - pC->point[pIndexedFaceSet->coordIndex[i0]] ) < FLT_EPSILON ) )
  {
    bool remove = true;
    if ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() )
    {
      DP_ASSERT( dynamic_cast<const Color *>(pIndexedFaceSet->color.get()) );
      const Color * pColor = static_cast<const Color *>(pIndexedFaceSet->color.get());
      remove =    ( pIndexedFaceSet->colorIndex[i0] == pIndexedFaceSet->colorIndex[i1] )
               || ( length( pColor->color[pIndexedFaceSet->colorIndex[i1]] - pColor->color[pIndexedFaceSet->colorIndex[i0]] ) < FLT_EPSILON );
      DP_ASSERT( remove );    // never encountered this
    }
    if ( remove && pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() )
    {
      DP_ASSERT( dynamic_cast<const Normal *>(pIndexedFaceSet->normal.get()) );
      const Normal * pNormal = static_cast<const Normal *>(pIndexedFaceSet->normal.get());
      remove =    ( pIndexedFaceSet->normalIndex[i0] == pIndexedFaceSet->normalIndex[i1] )
               || ( length( pNormal->vector[pIndexedFaceSet->normalIndex[i1]] - pNormal->vector[pIndexedFaceSet->normalIndex[i0]] ) < FLT_EPSILON );
    }
    if ( remove && pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() )
    {
      DP_ASSERT( dynamic_cast<const TextureCoordinate *>(pIndexedFaceSet->texCoord.get()) );
      const TextureCoordinate * pTextureCoordinate = static_cast<const TextureCoordinate *>(pIndexedFaceSet->texCoord.get());
      remove =    ( pIndexedFaceSet->texCoordIndex[i0] == pIndexedFaceSet->texCoordIndex[i1] )
               || ( length( pTextureCoordinate->point[pIndexedFaceSet->texCoordIndex[i1]] - pTextureCoordinate->point[pIndexedFaceSet->texCoordIndex[i0]] ) < FLT_EPSILON );
      DP_ASSERT( remove );    // never encountered this
    }

    if ( remove )
    {
      // two times the same index or the same position -> remove one of them
      pIndexedFaceSet->coordIndex.erase( pIndexedFaceSet->coordIndex.begin() + i1 );
      if ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() )
      {
        pIndexedFaceSet->colorIndex.erase( pIndexedFaceSet->colorIndex.begin() + i1 );
      }
      if ( pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() )
      {
        pIndexedFaceSet->normalIndex.erase( pIndexedFaceSet->normalIndex.begin() + i1 );
      }
      if ( pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() )
      {
        pIndexedFaceSet->texCoordIndex.erase( pIndexedFaceSet->texCoordIndex.begin() + i1 );
      }
      return( true );
    }
  }
  return( false );
}

void removeInvalidFace( SmartPtr<IndexedFaceSet> & pIndexedFaceSet, size_t i, size_t j, unsigned int numberOfFaces )
{
  pIndexedFaceSet->coordIndex.erase( pIndexedFaceSet->coordIndex.begin() + i
                                   , pIndexedFaceSet->coordIndex.begin() + i + j + 1 );
  if ( ! pIndexedFaceSet->colorIndex.empty() )
  {
    if ( pIndexedFaceSet->colorPerVertex )
    {
      pIndexedFaceSet->colorIndex.erase( pIndexedFaceSet->colorIndex.begin() + i
                                       , pIndexedFaceSet->colorIndex.begin() + i + j + 1 );
    }
    else
    {
      DP_ASSERT( numberOfFaces < pIndexedFaceSet->colorIndex.size() );
      pIndexedFaceSet->colorIndex.erase( pIndexedFaceSet->colorIndex.begin() + numberOfFaces );
    }
  }
  if ( ! pIndexedFaceSet->normalIndex.empty() )
  {
    if ( pIndexedFaceSet->normalPerVertex )
    {
      pIndexedFaceSet->normalIndex.erase( pIndexedFaceSet->normalIndex.begin() + i
                                        , pIndexedFaceSet->normalIndex.begin() + i + j + 1 );
    }
    else
    {
      DP_ASSERT( numberOfFaces < pIndexedFaceSet->normalIndex.size() );
      pIndexedFaceSet->normalIndex.erase( pIndexedFaceSet->normalIndex.begin() + numberOfFaces );
    }
  }
  if ( ! pIndexedFaceSet->texCoordIndex.empty() )
  {
    pIndexedFaceSet->texCoordIndex.erase( pIndexedFaceSet->texCoordIndex.begin() + i
                                        , pIndexedFaceSet->texCoordIndex.begin() + i + j + 1 );
  }
}

SmartPtr<IndexedFaceSet> WRLLoader::readIndexedFaceSet( const string &nodeName )
{
  SmartPtr<IndexedFaceSet> pIndexedFaceSet = new IndexedFaceSet;
  pIndexedFaceSet->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "color" )
    {
      readSFNode( pIndexedFaceSet, pIndexedFaceSet->color, getNextToken() );
      if ( pIndexedFaceSet->color )
      {
        if ( ! isSmartPtrOf<Color>( pIndexedFaceSet->color ) )
        {
          onUnsupportedToken( "IndexedFaceSet.color", pIndexedFaceSet->color->getType() );
          pIndexedFaceSet->color.reset();
        }
        else if ( static_cast<Color*>(pIndexedFaceSet->color.get())->color.empty() )
        {
          onEmptyToken( "IndexedFaceSet", "color" );
          pIndexedFaceSet->color.reset();
        }
      }
    }
    else if ( token == "coord" )
    {
      readSFNode( pIndexedFaceSet, pIndexedFaceSet->coord, getNextToken() );
      if ( pIndexedFaceSet->coord )
      {
        if ( ! isSmartPtrOf<Coordinate>( pIndexedFaceSet->coord ) )
        {
          onUnsupportedToken( "IndexedFaceSet.coord", pIndexedFaceSet->coord->getType() );
          pIndexedFaceSet->coord.reset();
        }
        else if ( static_cast<Coordinate*>(pIndexedFaceSet->coord.get())->point.empty() )
        {
          onEmptyToken( "IndexedFaceSet", "coord" );
          pIndexedFaceSet->coord.reset();
        }
      }
    }
    else if ( token == "normal" )
    {
      readSFNode( pIndexedFaceSet, pIndexedFaceSet->normal, getNextToken() );
      if ( pIndexedFaceSet->normal )
      {
        if ( ! isSmartPtrOf<Normal>( pIndexedFaceSet->normal ) )
        {
          onUnsupportedToken( "IndexedFaceSet.normal", pIndexedFaceSet->normal->getType() );
          pIndexedFaceSet->normal.reset();
        }
        else if ( static_cast<Normal*>(pIndexedFaceSet->normal.get())->vector.empty() )
        {
          onEmptyToken( "IndexedFaceSet", "normal" );
          pIndexedFaceSet->normal.reset();
        }
      }
    }
    else if ( token == "texCoord" )
    {
      readSFNode( pIndexedFaceSet, pIndexedFaceSet->texCoord, getNextToken() );
      if ( pIndexedFaceSet->texCoord )
      {
        if ( ! isSmartPtrOf<TextureCoordinate>( pIndexedFaceSet->texCoord ) )
        {
          onUnsupportedToken( "IndexedFaceSet.texCoord", pIndexedFaceSet->texCoord->getType() );
          pIndexedFaceSet->texCoord.reset();
        }
        else if ( static_cast<TextureCoordinate*>(pIndexedFaceSet->texCoord.get())->point.empty() )
        {
          onEmptyToken( "IndexedFaceSet", "texCoord" );
          pIndexedFaceSet->texCoord.reset();
        }
      }
    }
    else if ( token == "ccw" )
    {
      readSFBool( pIndexedFaceSet->ccw );
    }
    else if ( token == "colorIndex" )
    {
      readIndex( pIndexedFaceSet->colorIndex );
    }
    else if ( token == "colorPerVertex" )
    {
      readSFBool( pIndexedFaceSet->colorPerVertex );
    }
    else if ( token == "convex" )
    {
      readSFBool( pIndexedFaceSet->convex );
    }
    else if ( token == "coordIndex" )
    {
      readIndex( pIndexedFaceSet->coordIndex );
    }
    else if ( token == "creaseAngle" )
    {
      readSFFloat( pIndexedFaceSet->creaseAngle, getNextToken() );
    }
    else if ( token == "normalIndex" )
    {
      readIndex( pIndexedFaceSet->normalIndex );
    }
    else if ( token == "normalPerVertex" )
    {
      readSFBool( pIndexedFaceSet->normalPerVertex );
    }
    else if ( token == "solid" )
    {
      readSFBool( pIndexedFaceSet->solid );
    }
    else if ( token == "texCoordIndex" )
    {
      readIndex( pIndexedFaceSet->texCoordIndex );
    }
    else
    {
      onUnknownToken( "IndexedFaceSet", token );
    }
    token = getNextToken();
  }

  if ( pIndexedFaceSet->coord && ! pIndexedFaceSet->coordIndex.empty() )
  {
    //  if there's no -1 at the end, add one
    if ( pIndexedFaceSet->coordIndex[pIndexedFaceSet->coordIndex.size()-1] != -1 )
    {
      pIndexedFaceSet->coordIndex.push_back( -1 );
    }

    // count the number of faces
    unsigned int numberOfFaces = 0;
    int maxIndex = -1;
    for ( size_t i=0 ; i<pIndexedFaceSet->coordIndex.size() ; i++ )
    {
      if ( pIndexedFaceSet->coordIndex[i] == -1 )
      {
        numberOfFaces++;
      }
      else if ( maxIndex < pIndexedFaceSet->coordIndex[i] )
      {
        maxIndex = pIndexedFaceSet->coordIndex[i];
      }
    }

    // make sure color information is correct
    if ( pIndexedFaceSet->color )
    {
      //  if there's no -1 at the end, add one
      if (    pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty()
          &&  pIndexedFaceSet->colorIndex[pIndexedFaceSet->colorIndex.size()-1] != -1 )
      {
        pIndexedFaceSet->colorIndex.push_back( -1 );
      }

      if ( pIndexedFaceSet->colorPerVertex )
      {
        // first test on non-empty colorIndex
        if ( ! pIndexedFaceSet->colorIndex.empty() )
        {
          if ( pIndexedFaceSet->colorIndex.size() < pIndexedFaceSet->coordIndex.size() )
          {
            DP_ASSERT( pIndexedFaceSet->coordIndex.size() <= INT_MAX );
            DP_ASSERT( pIndexedFaceSet->colorIndex.size() <= INT_MAX );
            onIncompatibleValues( static_cast<int>(pIndexedFaceSet->coordIndex.size())
                                , static_cast<int>(pIndexedFaceSet->colorIndex.size())
                                , "IndexedFaceSet", "coordIndex.size", "colorIndex.size" );
            pIndexedFaceSet->colorIndex.clear();
          }
          else
          {
            for ( size_t i=0 ; i<pIndexedFaceSet->coordIndex.size() ; i++ )
            {
              if ( ( pIndexedFaceSet->coordIndex[i] < 0 ) ^ ( pIndexedFaceSet->colorIndex[i] < 0 ) )
              {
                onIncompatibleValues( pIndexedFaceSet->coordIndex[i], pIndexedFaceSet->colorIndex[i]
                                    , "IndexedFaceSet", "coordIndex", "colorIndex" );
                pIndexedFaceSet->colorIndex.clear();
                break;
              }
            }
          }
        }
        // retest colorIndex on emptiness: might be cleared above
        if ( pIndexedFaceSet->colorIndex.empty() )
        {
          DP_ASSERT( dynamic_cast<Color*>(pIndexedFaceSet->color.get())->color.size() <= INT_MAX );
          int maxColorIndex = checked_cast<int>(static_cast<Color*>(pIndexedFaceSet->color.get())->color.size());
          if ( maxColorIndex <= maxIndex )
          {
            onIncompatibleValues( maxIndex, maxColorIndex, "IndexedFaceSet", "coordIndex.size", "colors.max" );
            pIndexedFaceSet->color.reset();
          }
        }
      }
      else
      {
        // first test on non-empty colorIndex
        if ( ! pIndexedFaceSet->colorIndex.empty() )
        {
          if ( pIndexedFaceSet->colorIndex.size() < numberOfFaces )
          {
            DP_ASSERT( pIndexedFaceSet->colorIndex.size() <= INT_MAX );
            onIncompatibleValues( numberOfFaces, static_cast<int>(pIndexedFaceSet->colorIndex.size())
                                , "IndexedFaceSet", "faces.size", "colorIndex.size" );
            pIndexedFaceSet->colorIndex.clear();
          }
          else
          {
            for ( unsigned int i=0 ; i < numberOfFaces ; i++ )
            {
              if ( pIndexedFaceSet->colorIndex[i] < 0 )
              {
                onInvalidValue( pIndexedFaceSet->colorIndex[i], "IndexedFaceSet", "colorIndex" );
                pIndexedFaceSet->colorIndex.clear();
                break;
              }
            }
          }
        }
        // retest colorIndex on emptiness: might be cleared above
        if ( pIndexedFaceSet->colorIndex.empty() )
        {
          if ( dynamic_cast<Color*>(pIndexedFaceSet->color.get())->color.size() < numberOfFaces )
          {
            onIncompatibleValues( numberOfFaces
                                , checked_cast<int>(static_cast<Color*>(pIndexedFaceSet->color.get())->color.size())
                                , "IndexedFaceSet", "faces.size", "colors.size" );
            pIndexedFaceSet->color.reset();
          }
        }
      }
    }

    // make sure normal information is correct
    if ( pIndexedFaceSet->normal )
    {
      //  if there's no -1 at the end, add one
      if (    pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty()
          &&  pIndexedFaceSet->normalIndex[pIndexedFaceSet->normalIndex.size()-1] != -1 )
      {
        pIndexedFaceSet->normalIndex.push_back( -1 );
      }

      if ( pIndexedFaceSet->normalPerVertex )
      {
        // first test on non-empty normalIndex
        if ( ! pIndexedFaceSet->normalIndex.empty() )
        {
          if ( pIndexedFaceSet->normalIndex.size() < pIndexedFaceSet->coordIndex.size() )
          {
            DP_ASSERT( pIndexedFaceSet->coordIndex.size() <= INT_MAX );
            DP_ASSERT( pIndexedFaceSet->normalIndex.size() <= INT_MAX );
            onIncompatibleValues( static_cast<int>(pIndexedFaceSet->coordIndex.size())
                                , static_cast<int>(pIndexedFaceSet->normalIndex.size())
                                , "IndexedFaceSet", "coordIndex.size", "normalIndex.size" );
            pIndexedFaceSet->normalIndex.clear();
          }
          else
          {
            for ( size_t i=0 ; i<pIndexedFaceSet->coordIndex.size() ; i++ )
            {
              if ( ( pIndexedFaceSet->coordIndex[i] < 0 ) ^ ( pIndexedFaceSet->normalIndex[i] < 0 ) )
              {
                onIncompatibleValues( pIndexedFaceSet->coordIndex[i], pIndexedFaceSet->normalIndex[i]
                                    , "IndexedFaceSet", "coordIndex", "normalIndex" );
                pIndexedFaceSet->normalIndex.clear();
                break;
              }
            }
          }
        }
        // retest normalIndex on emptiness: might be cleared above
        if ( pIndexedFaceSet->normalIndex.empty() )
        {
          int maxNormalIndex = checked_cast<int>(static_cast<Normal*>(pIndexedFaceSet->normal.get())->vector.size());
          if ( maxNormalIndex <= maxIndex )
          {
            onIncompatibleValues( maxIndex, maxNormalIndex, "IndexedFaceSet", "coordIndex.max", "normals.size" );
            pIndexedFaceSet->normal.reset();
          }
        }
      }
      else
      {
        // first test on non-empty normalIndex
        if ( ! pIndexedFaceSet->normalIndex.empty() )
        {
          if ( pIndexedFaceSet->normalIndex.size() < numberOfFaces )
          {
            DP_ASSERT( pIndexedFaceSet->normalIndex.size() <= INT_MAX );
            onIncompatibleValues( numberOfFaces, static_cast<int>(pIndexedFaceSet->normalIndex.size())
                                , "IndexedFaceSet", "faces.size", "normalIndex.size" );
            pIndexedFaceSet->normalIndex.clear();
          }
          else
          {
            for ( unsigned int i=0 ; i < numberOfFaces ; i++ )
            {
              if ( pIndexedFaceSet->normalIndex[i] < 0 )
              {
                onInvalidValue( pIndexedFaceSet->normalIndex[i], "IndexedFaceSet", "normalIndex" );
                pIndexedFaceSet->normalIndex.clear();
                break;
              }
            }
          }
        }
        // retest normalIndex on emptiness: might be cleared above
        if ( pIndexedFaceSet->normalIndex.empty() )
        {
          if ( dynamic_cast<Normal*>(pIndexedFaceSet->normal.get())->vector.size() < numberOfFaces )
          {
            onIncompatibleValues( numberOfFaces
                                , checked_cast<int>(static_cast<Normal*>(pIndexedFaceSet->normal.get())->vector.size())
                                , "IndexedFaceSet", "faces.size", "normals.size" );
            pIndexedFaceSet->normal.reset();
          }
        }
      }
    }

    if ( pIndexedFaceSet->texCoord )
    {
      // first test on non-empty texCoordIndex
      if ( ! pIndexedFaceSet->texCoordIndex.empty() )
      {
        //  if there's no -1 at the end, add one
        if ( pIndexedFaceSet->texCoordIndex[pIndexedFaceSet->texCoordIndex.size()-1] != -1 )
        {
          pIndexedFaceSet->texCoordIndex.push_back( -1 );
        }
        if ( pIndexedFaceSet->texCoordIndex.size() < pIndexedFaceSet->coordIndex.size() )
        {
          DP_ASSERT( pIndexedFaceSet->coordIndex.size() <= INT_MAX );
          DP_ASSERT( pIndexedFaceSet->texCoordIndex.size() <= INT_MAX );
          onIncompatibleValues( static_cast<int>(pIndexedFaceSet->coordIndex.size())
                              , static_cast<int>(pIndexedFaceSet->texCoordIndex.size())
                              , "IndexedFaceSet", "coordIndex.size", "texCoordIndex.size" );
          pIndexedFaceSet->texCoordIndex.clear();
        }
        else
        {
          for ( size_t i=0 ; i<pIndexedFaceSet->coordIndex.size() ; i++ )
          {
            if ( ( pIndexedFaceSet->coordIndex[i] < 0 ) ^ ( pIndexedFaceSet->texCoordIndex[i] < 0 ) )
            {
              onIncompatibleValues( pIndexedFaceSet->coordIndex[i], pIndexedFaceSet->texCoordIndex[i]
                                  , "IndexedFaceSet", "coordIndex", "texCoordIndex" );
              pIndexedFaceSet->texCoordIndex.clear();
              break;
            }
          }
        }
      }
      // retest texCoordIndex on emptiness: might be cleared above
      if ( pIndexedFaceSet->texCoordIndex.empty() )
      {
        int maxTexCoordIndex = checked_cast<int>(static_cast<TextureCoordinate*>(pIndexedFaceSet->texCoord.get())->point.size());
        if ( maxTexCoordIndex <= maxIndex )
        {
          onIncompatibleValues( maxIndex, maxTexCoordIndex, "IndexedFaceSet", "coordIndex.max", "texCoord.size" );
          pIndexedFaceSet->texCoord.reset();
        }
      }
    }

    //  filter invalid indices
    int numberOfPoints = checked_cast<int>(static_cast<Coordinate*>(pIndexedFaceSet->coord.get())->point.size());
    int numberOfColors = pIndexedFaceSet->color ? checked_cast<int>(static_cast<Color*>(pIndexedFaceSet->color.get())->color.size()) : 0;
    int numberOfNormals = pIndexedFaceSet->normal ? checked_cast<int>(static_cast<Normal*>(pIndexedFaceSet->normal.get())->vector.size()) : 0;
    int numberOfTexCoords = pIndexedFaceSet->texCoord ? checked_cast<int>(static_cast<TextureCoordinate*>(pIndexedFaceSet->texCoord.get())->point.size()) : 0;
    for ( unsigned int i=0 ; i<pIndexedFaceSet->coordIndex.size() ; )
    {
      if ( pIndexedFaceSet->coordIndex[i] != -1 )
      {
        bool removeIndex = false;
        if ( numberOfPoints <= pIndexedFaceSet->coordIndex[i] )
        {
          onIncompatibleValues( pIndexedFaceSet->coordIndex[i], numberOfPoints, "IndexedFaceSet"
                              , "max( coordIndex )", "coord.size" );
          removeIndex = true;
        }
        if ( pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() )
        {
          DP_ASSERT( pIndexedFaceSet->colorIndex[i] != -1 );
          if ( numberOfColors <= pIndexedFaceSet->colorIndex[i] )
          {
            onIncompatibleValues( pIndexedFaceSet->colorIndex[i], numberOfColors, "IndexedFaceSet"
                                , "max( colorIndex )", "color.size" );
            removeIndex = true;
          }
        }
        if ( pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() )
        {
          DP_ASSERT( pIndexedFaceSet->normalIndex[i] != -1 );
          if ( numberOfNormals <= pIndexedFaceSet->normalIndex[i] )
          {
            onIncompatibleValues( pIndexedFaceSet->normalIndex[i], numberOfNormals, "IndexedFaceSet"
                                , "max( normalIndex )", "normal.size" );
            removeIndex = true;
          }
        }
        if ( ! pIndexedFaceSet->texCoordIndex.empty() )
        {
          DP_ASSERT( pIndexedFaceSet->texCoordIndex[i] != -1 );
          if ( numberOfTexCoords <= pIndexedFaceSet->texCoordIndex[i] )
          {
            onIncompatibleValues( pIndexedFaceSet->texCoordIndex[i], numberOfTexCoords, "IndexedFaceSet"
                                , "max( texCoordIndex )", "texCoord.size" );
            removeIndex = true;
          }
        }
        if ( removeIndex )
        {
          pIndexedFaceSet->coordIndex.erase( pIndexedFaceSet->coordIndex.begin() + i );
          if ( pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() )
          {
            pIndexedFaceSet->colorIndex.erase( pIndexedFaceSet->colorIndex.begin() + i );
          }
          if ( pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() )
          {
            pIndexedFaceSet->normalIndex.erase( pIndexedFaceSet->normalIndex.begin() + i );
          }
          if ( ! pIndexedFaceSet->texCoordIndex.empty() )
          {
            pIndexedFaceSet->texCoordIndex.erase( pIndexedFaceSet->texCoordIndex.begin() + i );
          }
        }
        else
        {
          i++;
        }
      }
      else
      {
        // assert that all other index arrays also have a -1 here
        DP_ASSERT(    ! pIndexedFaceSet->colorPerVertex
                    ||  pIndexedFaceSet->colorIndex.empty()
                    ||  ( pIndexedFaceSet->colorIndex[i] == -1 ) );
        DP_ASSERT(    ! pIndexedFaceSet->normalPerVertex
                    ||  pIndexedFaceSet->normalIndex.empty()
                    ||  ( pIndexedFaceSet->normalIndex[i] == -1 ) );
        DP_ASSERT(    pIndexedFaceSet->texCoordIndex.empty()
                    ||  ( pIndexedFaceSet->texCoordIndex[i] == -1 ) );
        i++;
      }
    }
    DP_ASSERT( !pIndexedFaceSet->coordIndex.empty() );

    //  filter invalid faces
    numberOfFaces = 0;
    for ( size_t i=0 ; i<pIndexedFaceSet->coordIndex.size() ; )
    {
      bool removeFace = false;
      // scan for next -1
      unsigned int j;
      for ( j=0 ; pIndexedFaceSet->coordIndex[i+j] != -1 ; j++ )
        ;

      if ( j < 3 )
      {
        onUnsupportedToken( "IndexedFaceSet", "n-gonal coordIndex with less than 3 coords" );
        removeFace = true;
      }
      if (    ! pIndexedFaceSet->colorPerVertex
          &&  ! pIndexedFaceSet->colorIndex.empty()
          &&  ( numberOfColors <= pIndexedFaceSet->colorIndex[numberOfFaces] ) )
      {
        onIncompatibleValues( pIndexedFaceSet->colorIndex[i], numberOfColors, "IndexedFaceSet"
                            , "max( colorIndex )", "color.size" );
        removeFace = true;
      }
      if (    ! pIndexedFaceSet->normalPerVertex
          &&  ! pIndexedFaceSet->normalIndex.empty()
          &&  ( numberOfNormals <= pIndexedFaceSet->normalIndex[numberOfFaces] ) )
      {
        onIncompatibleValues( pIndexedFaceSet->normalIndex[i], numberOfNormals, "IndexedFaceSet"
                            , "max( normalIndex )", "normal.size" );
        removeFace = true;
      }
      if (    ! pIndexedFaceSet->texCoordIndex.empty()
          &&  ( numberOfTexCoords <= pIndexedFaceSet->texCoordIndex[numberOfFaces] ) )
      {
        onIncompatibleValues( pIndexedFaceSet->texCoordIndex[i], numberOfTexCoords, "IndexedFaceSet"
                            , "max( texCoordIndex )", "texCoord.size" );
        removeFace = true;
      }
      if ( removeFace )
      {
        removeInvalidFace( pIndexedFaceSet, i, j, numberOfFaces );
      }
      else
      {
        i += j + 1;
        numberOfFaces++;
      }
    }

    //  some clean ups with invalid faces
    const Coordinate * pC = dynamic_cast<const Coordinate *>(pIndexedFaceSet->coord.get());
    DP_ASSERT( pIndexedFaceSet->coordIndex.size() <= UINT_MAX );
    DP_ASSERT( ! ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() ) || pIndexedFaceSet->colorIndex[0] != -1 );
    DP_ASSERT( ! ( pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() ) || pIndexedFaceSet->normalIndex[0] != -1 );
    DP_ASSERT( ! ( pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() ) || pIndexedFaceSet->texCoordIndex[0] != -1 );
    bool removed = false;
    for ( unsigned int i0=0, i=1 ; i<pIndexedFaceSet->coordIndex.size() ; i++ )
    {
      DP_ASSERT( pIndexedFaceSet->coordIndex[i-1] != -1 );
      if ( pIndexedFaceSet->coordIndex[i] == -1 )
      {
        DP_ASSERT( ! ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() ) || pIndexedFaceSet->colorIndex[i] == -1 );
        DP_ASSERT( ! ( pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() ) || pIndexedFaceSet->normalIndex[i] == -1 );
        DP_ASSERT( ! ( pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() ) || pIndexedFaceSet->texCoordIndex[i] == -1 );

        // end of face -> check against closed loop
        if ( ( i0 < i-1 ) && (    removeRedundantPoint( pIndexedFaceSet, i0, i-1 )
                              ||  ( ( i0 < i-2 ) && (     removeCollinearPoint( pIndexedFaceSet, i-2, i-1, i0 )
                                                      ||  removeCollinearPoint( pIndexedFaceSet, i-1, i0, i0+1 ) ) ) ) )
        {
          onUnsupportedToken( "IndexedFaceSet", "redundant point: removed" );
          // a redundant last point has been removed -> i now is the start of the next face, no additional advance of i
          i0 = i;
          removed = true;
        }
        else
        {
          // no redundant point has been removed -> i+1 now is the start of the next face, advance i one more
          i0 = i+1;
          i++;
        }

#if !defined( NDEBUG )
        if ( i0 < pIndexedFaceSet->coordIndex.size() )
        {
          DP_ASSERT( ! ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() ) || pIndexedFaceSet->colorIndex[i0] != -1 );
          DP_ASSERT( ! ( pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() ) || pIndexedFaceSet->normalIndex[i0] != -1 );
          DP_ASSERT( ! ( pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() ) || pIndexedFaceSet->texCoordIndex[i0] != -1 );
        }
#endif
      }
      else
      {
        DP_ASSERT( ! ( pIndexedFaceSet->color && pIndexedFaceSet->colorPerVertex && ! pIndexedFaceSet->colorIndex.empty() ) || pIndexedFaceSet->colorIndex[i] != -1 );
        DP_ASSERT( ! ( pIndexedFaceSet->normal && pIndexedFaceSet->normalPerVertex && ! pIndexedFaceSet->normalIndex.empty() ) || pIndexedFaceSet->normalIndex[i] != -1 );
        DP_ASSERT( ! ( pIndexedFaceSet->texCoord && ! pIndexedFaceSet->texCoordIndex.empty() ) || pIndexedFaceSet->texCoordIndex[i] != -1 );
        if (    removeRedundantPoint( pIndexedFaceSet, i-1, i )
            ||  ( ( i0 < i-1 ) && removeCollinearPoint( pIndexedFaceSet, i-2, i-1, i ) ) )
        {
          onUnsupportedToken( "IndexedFaceSet", "redundant point: removed" );
          i--;    // removed a redundant point -> back i by one
          removed = true;
        }
      }
    }

    // check again for faces with less than 3 vertices
    if ( removed )
    {
      numberOfFaces = 0;
      for ( size_t i=0 ; i<pIndexedFaceSet->coordIndex.size() ; )
      {
        bool removeFace = false;
        // scan for next -1
        unsigned int j;
        for ( j=0 ; pIndexedFaceSet->coordIndex[i+j] != -1 ; j++ )
          ;

        if ( j < 3 )
        {
          onUnsupportedToken( "IndexedFaceSet", "n-gonal coordIndex with less than 3 coords" );
          removeInvalidFace( pIndexedFaceSet, i, j, numberOfFaces );
        }
        else
        {
          i += j + 1;
          numberOfFaces++;
        }
      }
    }

    DP_ASSERT(    pIndexedFaceSet->colorIndex.empty()
                ||  ( pIndexedFaceSet->colorPerVertex
                      ? ( pIndexedFaceSet->colorIndex.size() == pIndexedFaceSet->coordIndex.size() )
                      : ( pIndexedFaceSet->colorIndex.size() == numberOfFaces ) ) );
    DP_ASSERT(    pIndexedFaceSet->normalIndex.empty()
                ||  ( pIndexedFaceSet->normalPerVertex
                      ? ( pIndexedFaceSet->normalIndex.size() == pIndexedFaceSet->coordIndex.size() )
                      : ( pIndexedFaceSet->normalIndex.size() == numberOfFaces ) ) );
    DP_ASSERT(    pIndexedFaceSet->texCoordIndex.empty()
                ||  ( pIndexedFaceSet->texCoordIndex.size() == pIndexedFaceSet->coordIndex.size() ) );
  }

  // check again, this might have changed above!
  if ( pIndexedFaceSet->coord && pIndexedFaceSet->coordIndex.empty() )
  {
    pIndexedFaceSet.reset();
  }

  return( pIndexedFaceSet );
}

SmartPtr<IndexedLineSet> WRLLoader::readIndexedLineSet( const string &nodeName )
{
  SmartPtr<IndexedLineSet> pIndexedLineSet( new IndexedLineSet );
  pIndexedLineSet->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "color" )
    {
      readSFNode( pIndexedLineSet, pIndexedLineSet->color, getNextToken() );
      if ( pIndexedLineSet->color && ! isSmartPtrOf<Color>( pIndexedLineSet->color ) )
      {
        onUnsupportedToken( "IndexedLineSet.color", pIndexedLineSet->color->getType() );
        pIndexedLineSet->color.reset();
      }
    }
    else if ( token == "coord" )
    {
      readSFNode( pIndexedLineSet, pIndexedLineSet->coord, getNextToken() );
      if ( pIndexedLineSet->coord && ! isSmartPtrOf<Coordinate>( pIndexedLineSet->coord ) )
      {
        onUnsupportedToken( "IndexedLineSet.coord", pIndexedLineSet->coord->getType() );
        pIndexedLineSet->coord.reset();
      }
    }
    else if ( token == "colorIndex" )
    {
      readIndex( pIndexedLineSet->colorIndex );
    }
    else if ( token == "colorPerVertex" )
    {
      readSFBool( pIndexedLineSet->colorPerVertex );
    }
    else if ( token == "coordIndex" )
    {
      readIndex( pIndexedLineSet->coordIndex );
    }
    else
    {
      onUnknownToken( "IndexedLineSet", token );
    }
    token = getNextToken();
  }

  return( pIndexedLineSet );
}

SmartPtr<Inline> WRLLoader::readInline( const string &nodeName )
{
  SmartPtr<Inline> pInline( new Inline );
  pInline->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "bboxCenter" )
    {
      readSFVec3f( pInline->bboxCenter, getNextToken() );
    }
    else if ( token == "bboxSize" )
    {
      readSFVec3f( pInline->bboxSize, getNextToken() );
    }
    else if ( token == "url" )
    {
      readMFString( pInline->url );
    }
    else
    {
      onUnknownToken( "Inline", token );
    }
    token = getNextToken();
  }

  // map multiply ref'ed Inlines on the same object
  std::map<MFString,dp::util::SmartPtr<Inline> >::const_iterator it = m_inlines.find( pInline->url );
  if ( it == m_inlines.end() )
  {
    it = m_inlines.insert( make_pair( pInline->url, pInline ) ).first;
  }
  else
  {
    DP_ASSERT( it->second->bboxCenter == pInline->bboxCenter );
    DP_ASSERT( it->second->bboxSize   == pInline->bboxSize );
  }

  return( it->second );
}

SmartPtr<vrml::LOD> WRLLoader::readLOD( const string &nodeName )
{
  SmartPtr<vrml::LOD> pLOD( new vrml::LOD );
  pLOD->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "level" )
    {
      readMFNode( pLOD );
    }
    else if ( token == "center" )
    {
      readSFVec3f( pLOD->center, getNextToken() );
    }
    else if ( token == "range" )
    {
      readMFFloat( pLOD->range );
    }
    else
    {
      onUnknownToken( "LOD", token );
    }
    token = getNextToken();
  }

  return( pLOD );
}

SmartPtr<vrml::Material> WRLLoader::readMaterial( const string &nodeName )
{
  SmartPtr<vrml::Material> pMaterial( new vrml::Material );
  pMaterial->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "ambientIntensity" )
    {
      readSFFloat( pMaterial->ambientIntensity, getNextToken() );
    }
    else if ( token == "diffuseColor" )
    {
      readSFColor( pMaterial->diffuseColor, getNextToken() );
    }
    else if ( token == "emissiveColor" )
    {
      readSFColor( pMaterial->emissiveColor, getNextToken() );
    }
    else if ( token == "shininess" )
    {
      readSFFloat( pMaterial->shininess, getNextToken() );
    }
    else if ( token == "specularColor" )
    {
      readSFColor( pMaterial->specularColor, getNextToken() );
    }
    else if ( token == "transparency" )
    {
      readSFFloat( pMaterial->transparency, getNextToken() );
    }
    else
    {
      onUnknownToken( "Material", token );
    }
    token = getNextToken();
  }

  return( pMaterial );
}

void  WRLLoader::readMFNode( const SmartPtr<vrml::Group> & fatherNode )
{
  SFNode n;
  string & token = getNextToken();
  if ( token == "[" )
  {
    token = getNextToken();
    while ( token != "]" )
    {
      readSFNode( fatherNode, n, token );
      if ( n )
      {
        fatherNode->children.push_back( n );
      }
      token = getNextToken();
      DP_ASSERT( !token.empty() );
    }
  }
  else
  {
    readSFNode( fatherNode, n, token );
    if ( n )
    {
      fatherNode->children.push_back( n );
    }
  }
}

template<typename SFType>
void  WRLLoader::readMFType( vector<SFType> &mf, void (WRLLoader::*readSFType)( SFType &sf, string &token ) )
{
  SFType  sf;
  string & token = getNextToken();
  if ( token == "[" )
  {
    token= getNextToken();
    while ( token != "]" )
    {
      (this->*readSFType)( sf, token );
      mf.push_back( sf );
      token = getNextToken();
    }
  }
  else
  {
    (this->*readSFType)( sf, token );
    mf.push_back( sf );
  }
}

SmartPtr<MovieTexture> WRLLoader::readMovieTexture( const string &nodeName )
{
  SmartPtr<MovieTexture> pMovieTexture( new MovieTexture );
  pMovieTexture->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "loop" )
    {
      readSFBool( pMovieTexture->loop );
    }
    else if ( token == "speed" )
    {
      readSFFloat( pMovieTexture->speed, getNextToken() );
    }
    else if ( token == "startTime" )
    {
      readSFTime( pMovieTexture->startTime );
    }
    else if ( token == "stopTime" )
    {
      readSFTime( pMovieTexture->stopTime );
    }
    else if ( token == "url" )
    {
      readMFString( pMovieTexture->url );
    }
    else if ( token == "repeatS" )
    {
      readSFBool( pMovieTexture->repeatS );
    }
    else if ( token == "repeatT" )
    {
      readSFBool( pMovieTexture->repeatT );
    }
    else
    {
      onUnknownToken( "MovieTexture", token );
    }
    token = getNextToken();
  }

  return( pMovieTexture );
}

SmartPtr<NavigationInfo> WRLLoader::readNavigationInfo( const string &nodeName )
{
  SmartPtr<NavigationInfo> pNavigationInfo = new NavigationInfo;
  pNavigationInfo->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "avatarSize" )
    {
      readMFFloat( pNavigationInfo->avatarSize );
    }
    else if ( token == "headlight" )
    {
      readSFBool( pNavigationInfo->headlight );
    }
    else if ( token == "speed" )
    {
      readSFFloat( pNavigationInfo->speed, getNextToken() );
    }
    else if ( token == "type" )
    {
      readMFString( pNavigationInfo->type );
    }
    else if ( token == "visibilityLimit" )
    {
      readSFFloat( pNavigationInfo->visibilityLimit, getNextToken() );
    }
    else
    {
      onUnknownToken( "NavigationInfo", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "NavigationInfo" );
  pNavigationInfo.reset();

  return( pNavigationInfo );
}

SmartPtr<Normal> WRLLoader::readNormal( const string &nodeName )
{
  SmartPtr<Normal> pNormal( new Normal );
  pNormal->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "vector" )
    {
      readMFVec3f( pNormal->vector );
      for ( size_t i=0 ; i<pNormal->vector.size() ; i++ )
      {
        pNormal->vector[i].normalize();
      }
    }
    else
    {
      onUnknownToken( "Normal", token );
    }
    token = getNextToken();
  }

  return( pNormal );
}

SmartPtr<NormalInterpolator> WRLLoader::readNormalInterpolator( const string &nodeName )
{
  SmartPtr<NormalInterpolator> pNormalInterpolator( new NormalInterpolator );
  pNormalInterpolator->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "key" )
    {
      readMFFloat( pNormalInterpolator->key );
    }
    else if ( token == "keyValue" )
    {
      readMFVec3f( pNormalInterpolator->keyValue );
    }
    else
    {
      onUnknownToken( "NormalInterpolator", token );
    }
    token = getNextToken();
  }

  DP_ASSERT( ( pNormalInterpolator->keyValue.size() % pNormalInterpolator->key.size() ) == 0 );

  return( pNormalInterpolator );
}

SmartPtr<OrientationInterpolator> WRLLoader::readOrientationInterpolator( const string &nodeName )
{
  SmartPtr<OrientationInterpolator> pOrientationInterpolator( new OrientationInterpolator );
  pOrientationInterpolator->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "key" )
    {
      readMFFloat( pOrientationInterpolator->key );
    }
    else if ( token == "keyValue" )
    {
      readMFRotation( pOrientationInterpolator->keyValue );
    }
    else
    {
      onUnknownToken( "OrientationInterpolator", token );
    }
    token = getNextToken();
  }

  DP_ASSERT( pOrientationInterpolator->key.size() == pOrientationInterpolator->keyValue.size() );

  return( pOrientationInterpolator );
}

SmartPtr<PixelTexture> WRLLoader::readPixelTexture( const string &nodeName )
{
  SmartPtr<PixelTexture> pPixelTexture( new PixelTexture );
  pPixelTexture->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "image" )
    {
      readSFImage( pPixelTexture->image );
    }
    else if ( token == "repeatS" )
    {
      readSFBool( pPixelTexture->repeatS );
    }
    else if ( token == "repeatT" )
    {
      readSFBool( pPixelTexture->repeatT );
    }
    else
    {
      onUnknownToken( "PixelTexture", token );
    }
    token = getNextToken();
  }

  return( pPixelTexture );
}

SmartPtr<PlaneSensor> WRLLoader::readPlaneSensor( const string &nodeName )
{
  SmartPtr<PlaneSensor> pPlaneSensor = new PlaneSensor;
  pPlaneSensor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "autoOffset" )
    {
      readSFBool( pPlaneSensor->autoOffset );
    }
    else if ( token == "enabled" )
    {
      readSFBool( pPlaneSensor->enabled );
    }
    else if ( token == "maxPosition" )
    {
      readSFVec2f( pPlaneSensor->maxPosition, getNextToken() );
    }
    else if ( token == "minPosition" )
    {
      readSFVec2f( pPlaneSensor->minPosition, getNextToken() );
    }
    else if ( token == "offset" )
    {
      readSFVec3f( pPlaneSensor->offset, getNextToken() );
    }
    else
    {
      onUnknownToken( "PlaneSensor", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "PlaneSensor" );
  pPlaneSensor.reset();

  return( pPlaneSensor );
}

SmartPtr<vrml::PointLight> WRLLoader::readPointLight( const string &nodeName )
{
  SmartPtr<vrml::PointLight> pPointLight( new vrml::PointLight );
  pPointLight->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "ambientIntensity" )
    {
      readSFFloat( pPointLight->ambientIntensity, getNextToken() );
    }
    else if ( token == "attenuation" )
    {
      readSFVec3f( pPointLight->attenuation, getNextToken() );
    }
    else if ( token == "color" )
    {
      readSFColor( pPointLight->color, getNextToken() );
    }
    else if ( token == "intensity" )
    {
      readSFFloat( pPointLight->intensity, getNextToken() );
    }
    else if ( token == "location" )
    {
      readSFVec3f( pPointLight->location, getNextToken() );
    }
    else if ( token == "on" )
    {
      readSFBool( pPointLight->on );
    }
    else if ( token == "radius" )
    {
      readSFFloat( pPointLight->radius, getNextToken() );
    }
    else
    {
      onUnknownToken( "PointLight", token );
    }
    token = getNextToken();
  }

  return( pPointLight );
}

SmartPtr<PointSet> WRLLoader::readPointSet( const string &nodeName )
{
  SmartPtr<PointSet> pPointSet( new PointSet );
  pPointSet->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "color" )
    {
      readSFNode( pPointSet, pPointSet->color, getNextToken() );
    }
    else if ( token == "coord" )
    {
      readSFNode( pPointSet, pPointSet->coord, getNextToken() );
    }
    else
    {
      onUnknownToken( "PointSet", token );
    }
    token = getNextToken();
  }

  return( pPointSet );
}

SmartPtr<PositionInterpolator> WRLLoader::readPositionInterpolator( const string &nodeName )
{
  SmartPtr<PositionInterpolator> pPositionInterpolator( new PositionInterpolator );
  pPositionInterpolator->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "key" )
    {
      readMFFloat( pPositionInterpolator->key );
    }
    else if ( token == "keyValue" )
    {
      readMFVec3f( pPositionInterpolator->keyValue );
    }
    else
    {
      onUnknownToken( "PositionInterpolator", token );
    }
    token = getNextToken();
  }

  DP_ASSERT( pPositionInterpolator->key.size() == pPositionInterpolator->keyValue.size() );

  return( pPositionInterpolator );
}

void  WRLLoader::readPROTO( void )
{
  onUnsupportedToken( "VRMLLoader", "PROTO" );
  m_PROTONames.insert( getNextToken() );    //  PrototypeName
  ignoreBlock( "[", "]", getNextToken() );  //  PrototypeDeclaration
  ignoreBlock( "{", "}", getNextToken() );  //  PrototypeDefinition
}

SmartPtr<ProximitySensor> WRLLoader::readProximitySensor( const string &nodeName )
{
  SmartPtr<ProximitySensor> pProximitySensor = new ProximitySensor;
  pProximitySensor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "center" )
    {
      readSFVec3f( pProximitySensor->center, getNextToken() );
    }
    else if ( token == "size" )
    {
      readSFVec3f( pProximitySensor->size, getNextToken() );
    }
    else if ( token == "enabled" )
    {
      readSFBool( pProximitySensor->enabled );
    }
    else
    {
      onUnknownToken( "ProximitySensor", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "ProximitySensor" );
  pProximitySensor.reset();

  return( pProximitySensor );
}

void  WRLLoader::readROUTE( const SFNode currentNode )
{
  bool ok = true;
  string from = getNextToken(); // no reference here!!!
  string & to = getNextToken(); // but reference allowed here.
  if ( to != "TO" )
  {
    onUnexpectedToken( "TO", to );
    ok = false;
  }
  else
  {
    to = getNextToken();
  }

  string  fromName, fromAction;
  SFNode  fromNode;
  if ( ok )
  {
    string::size_type sst = from.find( "." );
    if ( sst == string::npos )
    {
      onUnexpectedToken( "NodeName.eventOutName", from );
      ok = false;
    }
    else
    {
      fromName.assign( from, 0, sst );
      fromAction.assign( from, sst+1, string::npos );
      fromNode = findNode( currentNode, fromName );
    }
  }

  string  toName, toAction;
  SFNode  toNode;
  if ( ok )
  {
    string::size_type sst = to.find( "." );
    if ( sst == string::npos )
    {
      onUnexpectedToken( "NodeName.eventInName", to );
      ok = false;
    }
    else
    {
      toName.assign( to, 0, sst );
      toAction.assign( to, sst+1, string::npos );
      toNode = findNode( currentNode, toName );
    }
  }

  if ( ok )
  {
    if ( ! fromNode )
    {
      onUndefinedToken( "ROUTE nodeOutName", fromName );
    }
    else if ( toAction == "set_scaleOrientation" )
    {
      onUnsupportedToken( "ROUTE", toAction );
    }
    else if (   ( toAction == "set_scale" )
            &&  ! (   isSmartPtrOf<PositionInterpolator>(fromNode)
                   && isValidScaling( smart_cast<PositionInterpolator>(fromNode) ) ) )
    {
      onInvalidValue( 0, "ROUTE eventInName", toAction );
    }
    else if ( ! toNode )
    {
      onUndefinedToken( "ROUTE nodeInName", toName );
    }
    else
    {
      if ( isSmartPtrOf<ColorInterpolator>( fromNode ) )
      {
        if ( fromAction != "value_changed" )
        {
          onUnsupportedToken( "ROUTE eventOutName", fromAction );
        }
        else if ( ! isSmartPtrOf<Color>( toNode ) )
        {
          onUnsupportedToken( "ROUTE nodeInName", toNode->getType() );
        }
        else if ( ( toAction == "set_color" ) || ( toAction == "color" ) )
        {
          smart_cast<Color>(toNode)->set_color = smart_cast<ColorInterpolator>(fromNode);
        }
        else
        {
          onUnsupportedToken( "ROUTE eventInName", toAction );
        }
      }
      else if ( isSmartPtrOf<CoordinateInterpolator>( fromNode ) )
      {
        if ( fromAction != "value_changed" )
        {
          onUnsupportedToken( "ROUTE eventOutName", fromAction );
        }
        else if ( ! isSmartPtrOf<Coordinate>( toNode ) )
        {
          onUnsupportedToken( "ROUTE nodeInName", toNode->getType() );
        }
        else if ( ( toAction == "set_point" ) || ( toAction == "point" ) )
        {
          smart_cast<Coordinate>(toNode)->set_point = smart_cast<CoordinateInterpolator>(fromNode);
        }
        else
        {
          onUnsupportedToken( "ROUTE eventInName", toAction );
        }
      }
      else if ( isSmartPtrOf<NormalInterpolator>( fromNode ) )
      {
        if ( fromAction != "value_changed" )
        {
          onUnsupportedToken( "ROUTE eventOutName", fromAction );
        }
        else if ( ! isSmartPtrOf<Normal>( toNode ) )
        {
          onUnsupportedToken( "ROUTE nodeInName", toNode->getType() );
        }
        else if ( ( toAction == "set_vector" ) || ( toAction == "vector" ) )
        {
          smart_cast<Normal>(toNode)->set_vector = smart_cast<NormalInterpolator>(fromNode);
        }
        else
        {
          onUnsupportedToken( "ROUTE eventInName", toAction );
        }
      }
      else if ( isSmartPtrOf<OrientationInterpolator>( fromNode ) )
      {
        if ( fromAction != "value_changed" )
        {
          onUnsupportedToken( "ROUTE eventOutName", fromAction );
        }
        if ( isSmartPtrOf<vrml::Transform>( toNode ) )
        {
          if ( ( toAction == "set_rotation" ) || ( toAction == "rotation" ) )
          {
            smart_cast<vrml::Transform>(toNode)->set_rotation = smart_cast<OrientationInterpolator>(fromNode);
          }
          else
          {
            onUnsupportedToken( "ROUTE eventInName", toAction );
          }
        }
        else if ( isSmartPtrOf<Viewpoint>( toNode ) )
        {
          if ( ( toAction == "set_orientation" ) || ( toAction == "orientation" ) )
          {
            smart_cast<Viewpoint>(toNode)->set_orientation = smart_cast<OrientationInterpolator>(fromNode);
          }
          else
          {
            onUnsupportedToken( "ROUTE eventInName", toAction );
          }
        }
        else
        {
          onUnsupportedToken( "ROUTE nodeInName", toNode->getType() );
        }
      }
      else if ( isSmartPtrOf<PositionInterpolator>( fromNode ) )
      {
        if ( fromAction != "value_changed" )
        {
          onUnsupportedToken( "ROUTE eventOutName", fromAction );
        }
        if ( isSmartPtrOf<vrml::Transform>( toNode ) )
        {
          if ( ( toAction == "set_center" ) || ( toAction == "center" ) )
          {
            smart_cast<vrml::Transform>(toNode)->set_center = smart_cast<PositionInterpolator>(fromNode);
          }
          else if ( ( toAction == "set_scale" ) || ( toAction == "scale" ) )
          {
            smart_cast<vrml::Transform>(toNode)->set_scale = smart_cast<PositionInterpolator>(fromNode);
          }
          else if ( ( toAction == "set_translation" ) || ( toAction == "translation" ) )
          {
            smart_cast<vrml::Transform>(toNode)->set_translation = smart_cast<PositionInterpolator>(fromNode);
          }
          else
          {
            onUnsupportedToken( "ROUTE eventInName", toAction );
          }
        }
        else if ( isSmartPtrOf<Viewpoint>( toNode ) )
        {
          if ( ( toAction == "set_position" ) || ( toAction == "position" ) )
          {
            smart_cast<Viewpoint>(toNode)->set_position = smart_cast<PositionInterpolator>(fromNode);
          }
          else
          {
            onUnsupportedToken( "ROUTE eventInName", toAction );
          }
        }
        else
        {
          onUnsupportedToken( "ROUTE nodeInName", toNode->getType() );
        }
      }
      else if ( isSmartPtrOf<TimeSensor>( fromNode ) )
      {
        if ( fromAction != "fraction_changed" )
        {
          onUnsupportedToken( "ROUTE eventOutName", fromAction );
        }
        else if ( isSmartPtrOf<Interpolator>( toNode ) )
        {
          if ( ( toAction == "set_fraction" ) || ( toAction == "fraction" ) )
          {
            smart_cast<Interpolator>(toNode)->set_fraction = smart_cast<TimeSensor>(fromNode);
          }
          else
          {
            onUnsupportedToken( "ROUTE eventInName", toAction );
          }
        }
        else
        {
          onUnsupportedToken( "ROUTE nodeInName", toNode->getType() );
        }
      }
      else
      {
        onUnsupportedToken( "ROUTE nodeOutName", fromNode->getType() );
        ok = false;
      }
    }
  }
}

SmartPtr<ScalarInterpolator> WRLLoader::readScalarInterpolator( const string &nodeName )
{
  SmartPtr<ScalarInterpolator> pScalarInterpolator = new ScalarInterpolator;
  pScalarInterpolator->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "key" )
    {
      readMFFloat( pScalarInterpolator->key );
    }
    else if ( token == "keyValue" )
    {
      readMFFloat( pScalarInterpolator->keyValue );
    }
    else
    {
      onUnknownToken( "ScalarInterpolator", token );
    }
    token = getNextToken();
  }

  DP_ASSERT( pScalarInterpolator->key.size() == pScalarInterpolator->keyValue.size() );

  onUnsupportedToken( "VRMLLoader", "ScalarInterpolator" );
  pScalarInterpolator.reset();

  return( pScalarInterpolator );
}

SmartPtr<Script> WRLLoader::readScript( const string &nodeName )
{
#if 0
  DP_ASSERT( false );
  Script  * pScript = new Script;
  pScript->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "url" )
    {
      DP_ASSERT( false );
      readMFString( pScript->url );
    }
    else if ( token == "directOutput" )
    {
      DP_ASSERT( false );
      readSFBool( pScript->directOutput );
    }
    else if ( token == "mustEvaluate" )
    {
      DP_ASSERT( false );
      readSFBool( pScript->mustEvaluate );
    }
    else
    {
      onUnknownToken( "Script", token );
    }
    token = getNextToken();
  }

  return( pScript );
#else
  onUnsupportedToken( "VRMLLoader", "Script" );
  ignoreBlock( "{", "}", getNextToken() );
  return( SmartPtr<Script>() );
#endif
}

void  WRLLoader::ignoreBlock( const string &open, const string &close, string &token )
{
  onUnexpectedToken( open, token );
  token = getNextToken();
  while ( token != close )
  {
    if ( token == "{" )
    {
      ignoreBlock( "{", "}", token );
    }
    else if ( token == "[" )
    {
      ignoreBlock( "[", "]", token );
    }
    token = getNextToken();
  }
}

void  WRLLoader::readSFBool( SFBool &b )
{
  b = ( getNextToken() == "TRUE" );
}

void  WRLLoader::readSFColor( SFColor &c, string &token )
{
  readSFVec3f( c, token );
  c[0] = clamp( c[0], 0.0f, 1.0f );
  c[1] = clamp( c[1], 0.0f, 1.0f );
  c[2] = clamp( c[2], 0.0f, 1.0f );
}

void  WRLLoader::readSFFloat( SFFloat &f, string &token )
{
  f = _atof( token );
}

void  WRLLoader::readSFImage( SFImage &image )
{
  readSFInt32( image.width, getNextToken() );
  readSFInt32( image.height, getNextToken() );
  readSFInt32( image.numComponents, getNextToken() );
  switch( image.numComponents )
  {
    case 1 :
      image.pixelsValues = new SFInt8[image.width*image.height];
      for ( int i=0 ; i<image.height; i++ )
      {
        for ( int j=0 ; j<image.width ; j++ )
        {
          readSFInt8( image.pixelsValues[i*image.width+j] );
        }
      }
      break;
    case 3 :
    case 4 :
      {
        SFInt32 * pv = new SFInt32[image.width*image.height];
        for ( int i=0 ; i<image.height; i++ )
        {
          for ( int j=0 ; j<image.width ; j++ )
          {
            readSFInt32( pv[i*image.width+j], getNextToken() );
          }
        }
        image.pixelsValues = (SFInt8*) pv;
      }
      break;
    default:
      onInvalidValue( image.numComponents, "SFImage", "numComponents" );
      break;
  }

  onUnsupportedToken( "VRMLLoader", "SFImage" );
}

void  WRLLoader::readSFInt8( SFInt8 &i )
{
  i = atoi( getNextToken().c_str() );
}

void  WRLLoader::readSFInt32( SFInt32 &i, string &token )
{
  i = atoi( token.c_str() );
}

void  WRLLoader::readSFNode( const SFNode fatherNode, SFNode &n, string &token )
{
  m_openNodes.push_back( fatherNode );

  if ( token == "DEF" )
  {
    string nodeName = getNextToken();   // no reference here!!
    n = getNode( nodeName, getNextToken() );
    if ( n )
    {
      m_defNodes[nodeName] = n;
    }
  }
  else if ( token == "NULL" )
  {
    n = NULL;
  }
  else if ( token == "USE" )
  {
    string & nodeName = getNextToken();
    if ( m_defNodes.find( nodeName ) != m_defNodes.end() )
    {
      n = m_defNodes[nodeName];
    }
  }
  else
  {
    n = getNode( "", token );
  }

  DP_ASSERT( m_openNodes[m_openNodes.size()-1] == fatherNode );
  m_openNodes.pop_back();
}

void  WRLLoader::readSFRotation( SFRotation &r, string &token )
{
  SFVec3f axis;
  SFFloat angle;
  readSFVec3f( axis, token );
  readSFFloat( angle, getNextToken() );
  r = SFRotation( axis, angle );
}

void  WRLLoader::readSFString( SFString &s, string &token )
{
  if ( token[0] == '"' )
  {
    s = ( token.length() > 1 ) ? &token[1] : token = getNextToken();
    while ( token[token.length()-1] != '"' )
    {
      token = getNextToken();
      s += " ";
      s += token;
    }
    s.replace( s.find( "\"" ), 1, "" );
  }
  else
  {
    s = token;
  }
}

void  WRLLoader::readSFTime( SFTime &t )
{
  t = _atof( getNextToken() );
}

void  WRLLoader::readSFVec2f( SFVec2f &v, string &token )
{
  readSFFloat( v[0], token );
  readSFFloat( v[1], getNextToken() );
}

void  WRLLoader::readSFVec3f( SFVec3f &v, string &token )
{
  readSFFloat( v[0], token );
  readSFFloat( v[1], getNextToken() );
  readSFFloat( v[2], getNextToken() );
}

SmartPtr<vrml::Shape> WRLLoader::readShape( const string &nodeName )
{
  SmartPtr<vrml::Shape> pShape( new vrml::Shape );
  pShape->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "appearance" )
    {
      readSFNode( pShape, pShape->appearance, getNextToken() );
      if ( pShape->appearance && ! isSmartPtrOf<Appearance>( pShape->appearance ) )
      {
        onUnsupportedToken( "Shape.appearance", pShape->appearance->getType() );
        pShape->appearance.reset();
      }
    }
    else if ( token == "geometry" )
    {
      readSFNode( pShape, pShape->geometry, getNextToken() );
      if ( ! pShape->geometry )
      {
        onEmptyToken( "Shape", "geometry" );
      }
      else if ( ! isSmartPtrOf<vrml::Geometry>( pShape->geometry ) )
      {
        onUnsupportedToken( "Shape.geometry", pShape->geometry->getType() );
        pShape->geometry.reset();
      }
    }
    else
    {
      onUnknownToken( "Shape", token );
    }
    token = getNextToken();
  }

  return( pShape );
}

SmartPtr<Sound> WRLLoader::readSound( const string &nodeName )
{
  SmartPtr<Sound> pSound = new Sound;
  pSound->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "direction" )
    {
      readSFVec3f( pSound->direction, getNextToken() );
    }
    else if ( token == "intensity" )
    {
      readSFFloat( pSound->intensity, getNextToken() );
    }
    else if ( token == "location" )
    {
      readSFVec3f( pSound->location, getNextToken() );
    }
    else if ( token == "maxBack" )
    {
      readSFFloat( pSound->maxBack, getNextToken() );
    }
    else if ( token == "maxFront" )
    {
      readSFFloat( pSound->maxFront, getNextToken() );
    }
    else if ( token == "minBack" )
    {
      readSFFloat( pSound->minBack, getNextToken() );
    }
    else if ( token == "minFront" )
    {
      readSFFloat( pSound->minFront, getNextToken() );
    }
    else if ( token == "priority" )
    {
      readSFFloat( pSound->priority, getNextToken() );
    }
    else if ( token == "source" )
    {
      readSFNode( pSound, pSound->source, getNextToken() );
    }
    else if ( token == "spatialize" )
    {
      readSFBool( pSound->spatialize );
    }
    else
    {
      onUnknownToken( "Sound", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "Sound" );
  pSound.reset();

  return( pSound );
}

SmartPtr<Sphere> WRLLoader::readSphere( const string &nodeName )
{
  SmartPtr<Sphere> pSphere = new Sphere;
  pSphere->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "radius" )
    {
      readSFFloat( pSphere->radius, getNextToken() );
    }
    else
    {
      onUnknownToken( "Sphere", token );
    }
    token = getNextToken();
  }

  return( pSphere );
}

SmartPtr<SphereSensor> WRLLoader::readSphereSensor( const string &nodeName )
{
  SmartPtr<SphereSensor> pSphereSensor = new SphereSensor;
  pSphereSensor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "autoOffset" )
    {
      readSFBool( pSphereSensor->autoOffset );
    }
    else if ( token == "enabled" )
    {
      readSFBool( pSphereSensor->enabled );
    }
    else if ( token == "offset" )
    {
      readSFRotation( pSphereSensor->offset, getNextToken() );
    }
    else
    {
      onUnknownToken( "SphereSensor", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "SphereSensor" );
  pSphereSensor.reset();

  return( pSphereSensor );
}

SmartPtr<vrml::SpotLight> WRLLoader::readSpotLight( const string &nodeName )
{
  SmartPtr<vrml::SpotLight> pSpotLight( new vrml::SpotLight );
  pSpotLight->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "ambientIntensity" )
    {
      readSFFloat( pSpotLight->ambientIntensity, getNextToken() );
    }
    else if ( token == "attenuation" )
    {
      readSFVec3f( pSpotLight->attenuation, getNextToken() );
    }
    else if ( token == "beamWidth" )
    {
      readSFFloat( pSpotLight->beamWidth, getNextToken() );
    }
    else if ( token == "color" )
    {
      readSFColor( pSpotLight->color, getNextToken() );
    }
    else if ( token == "cutOffAngle" )
    {
      readSFFloat( pSpotLight->cutOffAngle, getNextToken() );
    }
    else if ( token == "direction" )
    {
      readSFVec3f( pSpotLight->direction, getNextToken() );
    }
    else if ( token == "intensity" )
    {
      readSFFloat( pSpotLight->intensity, getNextToken() );
    }
    else if ( token == "location" )
    {
      readSFVec3f( pSpotLight->location, getNextToken() );
    }
    else if ( token == "on" )
    {
      readSFBool( pSpotLight->on );
    }
    else if ( token == "radius" )
    {
      readSFFloat( pSpotLight->radius, getNextToken() );
    }
    else
    {
      onUnknownToken( "SpotLight", token );
    }
    token = getNextToken();
  }

  return( pSpotLight );
}

void  WRLLoader::readStatements( void )
{
  string & token = getNextToken();
  while ( !token.empty() )
  {
    if ( token == "EXTERNPROTO" )
    {
      readEXTERNPROTO();
    }
    else if ( token == "PROTO" )
    {
      readPROTO();
    }
    else if ( token == "ROUTE" )
    {
      readROUTE( NULL );
    }
    else
    {
      SFNode  n;
      readSFNode( NULL, n, token );
      if ( n )
      {
        m_topLevelGroup->children.push_back( n );
      }
    }
    token = getNextToken();
  }
}

SmartPtr<vrml::Switch> WRLLoader::readSwitch( const string &nodeName )
{
  SmartPtr<vrml::Switch> pSwitch( new vrml::Switch );
  pSwitch->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "choice" )
    {
      readMFNode( pSwitch );
    }
    else if ( token == "whichChoice" )
    {
      readSFInt32( pSwitch->whichChoice, getNextToken() );
    }
    else
    {
      onUnknownToken( "Switch", token );
    }
    token = getNextToken();
  }

  return( pSwitch );
}

SmartPtr<Text> WRLLoader::readText( const string &nodeName )
{
  SmartPtr<Text> pText = new Text;
  pText->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "string" )
    {
      readMFString( pText->string );
    }
    else if ( token == "fontStyle" )
    {
      readSFNode( pText, pText->fontStyle, getNextToken() );
    }
    else if ( token == "length" )
    {
      readMFFloat( pText->length );
    }
    else if ( token == "maxExtent" )
    {
      readSFFloat( pText->maxExtent, getNextToken() );
    }
    else
    {
      onUnknownToken( "Text", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "Text" );
  pText.reset();

  return( pText );
}

SmartPtr<TextureCoordinate> WRLLoader::readTextureCoordinate( const string &nodeName )
{
  SmartPtr<TextureCoordinate> pTextureCoordinate( new TextureCoordinate );
  pTextureCoordinate->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "point" )
    {
      readMFVec2f( pTextureCoordinate->point );
    }
    else
    {
      onUnknownToken( "TextureCoordinate", token );
    }
    token = getNextToken();
  }

  return( pTextureCoordinate );
}

SmartPtr<TextureTransform> WRLLoader::readTextureTransform( const string &nodeName )
{
  SmartPtr<TextureTransform> pTextureTransform( new TextureTransform );
  pTextureTransform->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "center" )
    {
      readSFVec2f( pTextureTransform->center, getNextToken() );
    }
    else if ( token == "rotation" )
    {
      readSFFloat( pTextureTransform->rotation, getNextToken() );
    }
    else if ( token == "scale" )
    {
      readSFVec2f( pTextureTransform->scale, getNextToken() );
    }
    else if ( token == "translation" )
    {
      readSFVec2f( pTextureTransform->translation, getNextToken() );
    }
    else
    {
      onUnknownToken( "TextureTransform", token );
    }
    token = getNextToken();
  }

  return( pTextureTransform );
}

SmartPtr<TimeSensor> WRLLoader::readTimeSensor( const string &nodeName )
{
  SmartPtr<TimeSensor> pTimeSensor( new TimeSensor );
  pTimeSensor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "cycleInterval" )
    {
      readSFTime( pTimeSensor->cycleInterval );
    }
    else if ( token == "enabled" )
    {
      readSFBool( pTimeSensor->enabled );
    }
    else if ( token == "loop" )
    {
      readSFBool( pTimeSensor->loop );
    }
    else if ( token == "startTime" )
    {
      readSFTime( pTimeSensor->startTime );
    }
    else if ( token == "stopTime" )
    {
      readSFTime( pTimeSensor->stopTime );
    }
    else
    {
      onUnknownToken( "TimeSensor", token );
    }
    token = getNextToken();
  }

  return( pTimeSensor );
}

SmartPtr<TouchSensor> WRLLoader::readTouchSensor( const string &nodeName )
{
  SmartPtr<TouchSensor> pTouchSensor = new TouchSensor;
  pTouchSensor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "enabled" )
    {
      readSFBool( pTouchSensor->enabled );
    }
    else
    {
      onUnknownToken( "TouchSensor", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "TouchSensor" );
  pTouchSensor.reset();

  return( pTouchSensor );
}

SmartPtr<vrml::Transform> WRLLoader::readTransform( const string &nodeName )
{
  SmartPtr<vrml::Transform> pTransform = new vrml::Transform;
  pTransform->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "center" )
    {
      readSFVec3f( pTransform->center, getNextToken() );
    }
    else if ( token == "children" )
    {
      readMFNode( pTransform.get() );
    }
    else if ( token == "rotation" )
    {
      readSFRotation( pTransform->rotation, getNextToken() );
    }
    else if ( token == "scale" )
    {
      readSFVec3f( pTransform->scale, getNextToken() );
    }
    else if ( token == "scaleOrientation" )
    {
      readSFRotation( pTransform->scaleOrientation, getNextToken() );
    }
    else if ( token == "translation" )
    {
      readSFVec3f( pTransform->translation, getNextToken() );
    }
    else if ( token == "ROUTE" )
    {
      readROUTE( pTransform );
    }
    else
    {
      onUnknownToken( "Transform", token );
    }
    token = getNextToken();
  }

  if ( ! isValidScaling( pTransform->scale ) )
  {
    pTransform.reset();
  }

  return( pTransform );
}

SmartPtr<Viewpoint> WRLLoader::readViewpoint( const string &nodeName )
{
  SmartPtr<Viewpoint> pViewpoint( new Viewpoint() );
  pViewpoint->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "fieldOfView" )
    {
      readSFFloat( pViewpoint->fieldOfView, getNextToken() );
      DP_ASSERT( ( 0.0f < pViewpoint->fieldOfView ) && ( pViewpoint->fieldOfView < PI ) );
    }
    else if ( token == "jump" )
    {
      readSFBool( pViewpoint->jump );
    }
    else if ( token == "orientation" )
    {
      readSFRotation( pViewpoint->orientation, getNextToken() );
    }
    else if ( token == "position" )
    {
      readSFVec3f( pViewpoint->position, getNextToken() );
    }
    else if ( token == "description" )
    {
      readSFString( pViewpoint->description, getNextToken() );
    }
    else
    {
      onUnknownToken( "Viewpoint", token );
    }
    token = getNextToken();
  }

  return( pViewpoint );
}

SmartPtr<VisibilitySensor> WRLLoader::readVisibilitySensor( const string &nodeName )
{
  SmartPtr<VisibilitySensor> pVisibilitySensor = new VisibilitySensor;
  pVisibilitySensor->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "center" )
    {
      readSFVec3f( pVisibilitySensor->center, getNextToken() );
    }
    else if ( token == "enabled" )
    {
      readSFBool( pVisibilitySensor->enabled );
    }
    else if ( token == "size" )
    {
      readSFVec3f( pVisibilitySensor->size, getNextToken() );
    }
    else
    {
      onUnknownToken( "VisibilitySensor", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "VisibilitySensor" );
  pVisibilitySensor.reset();

  return( pVisibilitySensor );
}

SmartPtr<WorldInfo> WRLLoader::readWorldInfo( const string &nodeName )
{
  SmartPtr<WorldInfo> pWorldInfo = new WorldInfo;
  pWorldInfo->setName( nodeName );

  string & token = getNextToken();
  onUnexpectedToken( "{", token );
  token = getNextToken();
  while ( token != "}" )
  {
    if ( token == "info" )
    {
      readMFString( pWorldInfo->info );
    }
    else if ( token == "title" )
    {
      readSFString( pWorldInfo->title, getNextToken() );
    }
    else
    {
      onUnknownToken( "WorldInfo", token );
    }
    token = getNextToken();
  }

  onUnsupportedToken( "VRMLLoader", "WorldInfo" );
  pWorldInfo.reset();

  return( pWorldInfo );
}

void WRLLoader::setNextToken( void )
{
  if ( ( m_nextTokenEnd == string::npos ) && !m_eof )
  {
    getNextLine();
    m_nextTokenEnd = 0;
  }
  if ( !m_eof )
  {
    DP_ASSERT( !m_currentString.empty() );
    do
    {
      m_nextTokenStart = findNotDelimiter( m_currentString, m_nextTokenEnd );   // find_first_not_of is slower!
      if ( ( m_nextTokenStart == string::npos ) || ( m_currentString[m_nextTokenStart] == '#' ) )
      {
        getNextLine();
        m_nextTokenEnd = m_eof ? string::npos : 0;
      }
      else
      {
        m_nextTokenEnd = findDelimiter( m_currentString, m_nextTokenStart );    // find_first_of is slower!
      }
    } while ( m_nextTokenEnd == 0 );
  }
}

bool  WRLLoader::testWRLVersion( const string &filename )
{
  m_lineNumber = 0;
  bool ok = getNextLine();
  if ( ok )
  {
    if ( m_currentString.compare( 0, 15, "#VRML V2.0 utf8" ) == 0 )
    {
      m_currentString.clear();
      m_nextTokenEnd = string::npos;
      setNextToken();
    }
    else if ( m_currentString.compare( 0, 16, "#VRML V1.0 ascii" ) == 0 )
    {
      if ( callback() )
      {
        callback()->onIncompatibleFile( filename, "VRML", 2<<16, 1<<16 );
      }
    }
    else
    {
      if ( callback() )
      {
        callback()->onInvalidFile( filename, "VRML" );
      }
    }
  }
  else if ( callback() )
  {
    callback()->onUnexpectedEndOfFile( m_lineNumber );
  }
  return( ok );
}

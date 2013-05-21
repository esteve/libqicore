/**
* @author Aldebaran Robotics
* Aldebaran Robotics (c) 2012 All Rights Reserved
*/

#include <queue>

#include <alerror/alerror.h>
#include <almath/tools/altrigonometry.h>
#include <almathinternal/interpolations/alinterpolation.h>
#include <alcommon/alproxy.h>

#include <qi/log.hpp>

#include "timeline_p.hpp"
#include <qicore-compat/timeline.hpp>
#include <qicore-compat/model/actuatorlistmodel.hpp>
#include <qicore-compat/model/actuatorcurvemodel.hpp>
#include <qicore-compat/model/keymodel.hpp>
#include <qicore-compat/model/tangentmodel.hpp>

namespace qi
{

TimelinePrivate::TimelinePrivate(ObjectPtr memory, ObjectPtr motion)
  : _executer(new asyncExecuter(1000 / 25)),
    _fps(0),
    _enabled(false),
    _startFrame(0),
    _endFrame(-1),
    _lastFrame(-1),
    _currentDoInterpolationMoveOrderId(-1),
    _name("Timeline"),
    _resourcesAcquisition(AnimationModel::MotionResourcesHandler_Passive),
    _methodMonitor(),
    _framesFlagsMap()
{
  try
  {
    boost::shared_ptr<AL::ALProxy> proxyMemory(new AL::ALProxy(memory, "ALMemory"));
    boost::shared_ptr<AL::ALProxy> proxyMotion(new AL::ALProxy(motion, "ALMotion"));
    _memoryProxy = boost::shared_ptr<AL::ALMemoryProxy>(new AL::ALMemoryProxy(proxyMemory));
    _motionProxy = boost::shared_ptr<AL::ALMotionProxy>(new AL::ALMotionProxy(proxyMotion));
  }
  catch (AL::ALError& e)
  {
    qiLogError("Timeline") << "Cannot create proxy on ALMotion :" << std::endl << e.toString() << std::endl;
  }
}

TimelinePrivate::~TimelinePrivate(void)
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);

  stop();
  try
  {
    _memoryProxy->removeMicroEvent(_name);
  }
  catch(AL::ALError&)
  {
  }

  delete _executer;
}

void TimelinePrivate::killMotionOrders()
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);

  // stop curves
  try
  {
    if(_currentDoInterpolationMoveOrderId != -1)
    {
      _motionProxy->stop(_currentDoInterpolationMoveOrderId);
      _currentDoInterpolationMoveOrderId = -1;
    }
  }
  catch(AL::ALError& e)
  {
    qiLogError("Timeline") << _name << e.toString() << std::endl;
  }
}

void TimelinePrivate::play()
{
  qiLogDebug("qiCore.Timeline") << "Play timeline";
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);

  if (_enabled == false || _fps == 0)
  {
    if (_currentFrame == _startFrame)
      ++_currentFrame;
    return;
  }

  _executer->playExecuter(boost::bind(&TimelinePrivate::update, this));
}

void TimelinePrivate::pause()
{
  qiLogDebug("qiCore.Timeline") << "Pause timeline";
  _executer->waitUntilPauseExecuter();
  killMotionOrders();
}

void TimelinePrivate::stop()
{
  qiLogDebug("qiCore.Timeline") << "Stopping timeline";
  _executer->stopExecuter();

  {
    boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);

    killMotionOrders();
    _currentFrame = -1;
    _currentFrame = _startFrame;
  }

  qiLogDebug("qiCore.Timeline") << "Timeline stopped";
}

void TimelinePrivate::goTo(int pFrame)
{
  qiLogDebug("qiCore.Timeline") << "goto timeline with : " << pFrame;
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  if (!_enabled)
    return;
  _currentFrame = pFrame;
  killMotionOrders();
}

int TimelinePrivate::getSize() const
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  return _endFrame - _startFrame;
}

void TimelinePrivate::setFPS(int pFps)
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);

  _fps = pFps;
  _executer->setInterval(1000 / _fps);
}

int TimelinePrivate::getFPS() const
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  return _fps;
}

void TimelinePrivate::setAnimation(boost::shared_ptr<AnimationModel> anim)
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);

  if(!anim)
    return;

  if(_executer->isPlaying())
    stop();

  _actuatorCurves.clear();

  _name       = anim->path();
  _fps        = anim->fps();
  _enabled    = true;
  _startFrame = anim->startFrame();
  _endFrame   = anim->endFrame();

  _currentFrame = _startFrame;
  _executer->setInterval(1000 / _fps);

  _resourcesAcquisition = anim->resourcesAcquisition();

  _lastFrame = 0;
  std::list<ActuatorCurveModelPtr> list = anim->actuatorList()->actuatorsCurve();
  std::list<ActuatorCurveModelPtr>::const_iterator it    = list.begin();
  std::list<ActuatorCurveModelPtr>::const_iterator itEnd = list.end();
  for(; it != itEnd; ++it)
  {
    ActuatorCurveModelPtr curve = *it;

    if( !curve->mute() )
    {
      int lastKeyFrame = curve->lastKeyFrame();
      _actuatorCurves.push_back(curve);
      if( lastKeyFrame > _lastFrame )
        _lastFrame = lastKeyFrame;
    }
    rebuildBezierAutoTangents(curve);
  }

  // if _endFrame = -1, meaning end frame is "undefined" => attached to last motion keyframe
  // then use last motion keyframe to stop timeline
  if(_endFrame == -1)
    _endFrame = _lastFrame;
}

bool TimelinePrivate::update(void)
{
  boost::unique_lock<boost::recursive_mutex> _lock(_methodMonitor);

  /* Send commands to the StateMachine if needed */
  /*if (_stateMachine)
  {
    std::map<int, std::string>::const_iterator it = _framesFlagsMap.find(_currentFrame);

    if (it != _framesFlagsMap.end())
      _stateMachine->goToStateName(it->second);
  }*/

  if (_enabled == false)
  {
    // update is not necessary
    return true;
  }

  // if on the end frame
  if ( ((_endFrame >= 1) && (_currentFrame >= _endFrame))
    || (_currentFrame < _startFrame))
  {
    updateFrameInSTM();
    _currentFrame = _startFrame;
    killMotionOrders();

    qiLogDebug("qiCore.Timeline") << "Timeline is done, invoking callback";

    return false;
  }

  // try to call motion commands
  bool motionWorked = executeCurveMotionCommand();

  // move on to the next frame
  if(motionWorked && _executer->isPlaying())
    ++_currentFrame;
  return true;
}

void TimelinePrivate::updateFrameInSTM()
{
  // set the value of the current frame for this timeline in the stm
  if(!_name.empty())
  {
    try
    {
      _memoryProxy->raiseMicroEvent(_name, (int) _currentFrame);
    }
    catch(AL::ALError& e)
    {
      qiLogError("Timeline") << _name << " Error During STM access : Error #=" << e.toString() << std::endl;
    }
  }
}

bool TimelinePrivate::executeCurveMotionCommand()
{
  if (_currentDoInterpolationMoveOrderId != -1)
  {
    // no need to do anything, the command was already sent before or we are at the last keyframe
    return true;
  }

  // send new command to motion
  bool isprepared = false;
  if (_executer->isPlaying())
    isprepared = prepareInterpolationCommand(_currentFrame);
  else
    isprepared = singleInterpolationCommand(_currentFrame);

  return isprepared;
}

bool TimelinePrivate::singleInterpolationCommand(int currentFrame)
{
  std::vector<std::string> actuatorNames;
  std::vector<float> actuatorValues;

  for (size_t i=0; i < _actuatorCurves.size(); ++i)
  {
    ActuatorCurveModelPtr actuatorCurve = _actuatorCurves[i];

    // FIXME : speedLimit
    float speedLimit = std::numeric_limits<float>::max();
    float valueIncrementLimit = speedLimit/_fps;

    float interpolatedValue = getInterpolatedValue(*actuatorCurve, currentFrame, valueIncrementLimit);
    float actuatorValue     = getMotionValue(*actuatorCurve, interpolatedValue);

    actuatorNames.push_back(actuatorCurve->actuator());
    actuatorValues.push_back(actuatorValue);
  }

  if(actuatorNames.empty())
    return true;

  // FIXME : motorSpeed
  try
  {
    _motionProxy->post.angleInterpolationWithSpeed(actuatorNames, actuatorValues, 0.25f);
  }
  catch(const AL::ALError& e)
  {
    qiLogError("Timeline") << _name << " Error during .postAngleInterpolationWithSpeed in motion : Error #= " << e.toString() << std::endl;
  }
  return true;
}

bool TimelinePrivate::prepareInterpolationCommand(int startFrame)
{
  std::vector<std::string> names;
  AL::ALValue times;
  AL::ALValue keys;

  size_t size = _actuatorCurves.size();

  // EPOT FS#1967 : at the moment, Motion does not accept empty commands...
  // Therefore, we only add actuators that are modified in the command we send.

  for (size_t i=0; i<size; ++i)
  {
    ActuatorCurveModelPtr curve = _actuatorCurves[i];

    AL::ALValue times_i;
    AL::ALValue keys_i;

    // filters keys after start frame
    std::map<int, KeyModelPtr> noFiltredkeys = curve->keys();
    std::deque<std::pair<int, KeyModelPtr> > filteredKeys;
    remove_copy_if(noFiltredkeys.begin(), noFiltredkeys.end(), back_inserter(filteredKeys),
        boost::bind(&std::map<int, KeyModelPtr>::value_type::first, _1) < startFrame );

    // for each key
    size_t size = filteredKeys.size();

    // if there are nothing to change for this actuator from current frame to the end,
    // then just pass to next actuator.
    if(size == 0)
      continue;

    names.push_back(curve->actuator());

    float scale;
    switch(curve->unit())
    {
    case ActuatorCurveModel::UnitType_Degree:
      scale = AL::Math::TO_RAD;
      break;

    case ActuatorCurveModel::UnitType_Percent:
      scale = 1.f;
      break;

    // backport compatibility
    case ActuatorCurveModel::UnitType_Undefined:
    default:
      if(curve->actuator() =="LHand" || curve->actuator()=="RHand")
        scale = 1.f;
      else
        scale = AL::Math::TO_RAD;
    }

    times_i.arraySetSize(size);
    keys_i.arraySetSize(size);
    for (size_t j=0; j<size; ++j)
    {
      int frame = filteredKeys[j].first;
      KeyModelPtr key = filteredKeys[j].second;

      // build command parameters for that key
      times_i[j] = float(frame - startFrame) / _fps;
      keys_i[j] = AL::ALValue::array(
            key->value() * scale,
            AL::ALValue::array(key->leftTangent()->interpType(),
                               key->leftTangent()->abscissaParam() / _fps,
                               key->leftTangent()->ordinateParam() * scale),
            AL::ALValue::array(key->rightTangent()->interpType(),
                               key->rightTangent()->abscissaParam() / _fps,
                               key->rightTangent()->ordinateParam() * scale)
            );
    }
    times.arrayPush(times_i);
    keys.arrayPush(keys_i);
  }

  if(names.empty())
    return true;

  return sendInterpolationCommand(names, times, keys);
}

bool TimelinePrivate::sendInterpolationCommand(const std::vector<std::string>& names, const AL::ALValue& times, const AL::ALValue& keys)
{
  if(_resourcesAcquisition != AnimationModel::MotionResourcesHandler_Passive)
  {
    // Ask to know if the order prepared is possible.
    try
    {
      if(!_motionProxy->areResourcesAvailable(names))
      {
        if(_resourcesAcquisition == AnimationModel::MotionResourcesHandler_Waiting)
          return false; // we will not execute anything, and we just wait fot another turn.

        // If in aggressive mode, then kill all tasks using the same resources, mouahahahahah !
        if(_resourcesAcquisition == AnimationModel::MotionResourcesHandler_Aggressive)
          _motionProxy->killTasksUsingResources(names);
      }
    }
    catch(AL::ALError& e)
    {
      // do nothing, just keep reading the timeline !
      qiLogError("Timeline") << _name << ":sendInterpolationCommand failed with the error:\n" << e.toString() << std::endl;
    }
  }

  try
  {
    _currentDoInterpolationMoveOrderId = _motionProxy->post.angleInterpolationBezier(names, times, keys);
  }
  catch(AL::ALError& e)
  {
    qiLogError("Timeline") << _name << " Error during .angleInterpolationBezier in motion : Error #= " << e.toString() << std::endl;
  }

  return true;
}

bool TimelinePrivate::getEnabled() const
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  return _enabled;
}

int TimelinePrivate::getCurrentFrame() const
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  return _currentFrame;
}

void TimelinePrivate::setCurrentFrame(int pFrame)
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  if(pFrame < 0)
    return;
  _currentFrame = pFrame;
}

void TimelinePrivate::setName(const std::string& var)
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  _name = var;

  // insert value of frame = -1 in the stm (used by choregraphe)
  if(_name != "")
  {
    try
    {
      _memoryProxy->raiseMicroEvent(_name, (int) -1);
    }
    catch(AL::ALError& e)
    {
      qiLogError("Timeline") << _name << " Error During STM access : Error #=" << e.toString() << std::endl;
    }
  }
}

std::string TimelinePrivate::getName() const
{
  boost::unique_lock<boost::recursive_mutex> lock(_methodMonitor);
  return _name;
}

// Returns the nearest neighbor (right and left) key frames in the timeline for a given key frame
// if given key is before the first key, both neighbors are set to this first key
// if after the last key, both neighbors are set to this last key
void TimelinePrivate::getNeighborKeysOf(const ActuatorCurveModel& curve,
                                        const int indexKey,
                                        int& indexLeftKey,
                                        KeyModelPtr& leftKey,
                                        int& indexRightKey,
                                        KeyModelPtr& rightKey)
{
  const std::map<int, KeyModelPtr>& keys = curve.keys();

  if (keys.empty())
  {
    indexLeftKey = -1;
    indexRightKey = -1;
    return;
  }

  std::map<int, KeyModelPtr>::const_iterator it = keys.begin();

  indexLeftKey = it->first;
  leftKey = it->second;

  if (indexKey > indexLeftKey)
  {
    while (it != keys.end())
    {
      indexRightKey = it->first;
      rightKey = it->second;
      if (indexKey < indexRightKey)
      {
        return;
      }
      indexLeftKey = indexRightKey;
      leftKey = rightKey;
      it++;
    }
  }
  else
  {
    // given key before first key => neighbors are both this first key
    indexRightKey = indexLeftKey;
    rightKey = leftKey;
  }
}

// Returns the value for a given frame based on interpolation on the two nearest frames
float TimelinePrivate::getInterpolatedValue(const ActuatorCurveModel& curve,
                                            const int& indexKey,
                                            const float& valueIncrementLimit)
{
  AL::Math::Interpolation::ALInterpolationBezier interpolator;

  int indexLeftKey;
  KeyModelPtr lKey;
  int indexRightKey;
  KeyModelPtr rKey;


  getNeighborKeysOf(curve, indexKey, indexLeftKey, lKey, indexRightKey, rKey);

  AL::Math::Interpolation::Key leftKey(lKey->value(),
                                       AL::Math::Interpolation::Tangent(static_cast<AL::Math::Interpolation::InterpolationType>(lKey->leftTangent()->interpType()),
                                                                        AL::Math::Position2D(lKey->leftTangent()->abscissaParam(),
                                                                                             lKey->leftTangent()->ordinateParam())),
                                       AL::Math::Interpolation::Tangent(static_cast<AL::Math::Interpolation::InterpolationType>(lKey->rightTangent()->interpType()),
                                                                        AL::Math::Position2D(lKey->rightTangent()->abscissaParam(),
                                                                                             lKey->rightTangent()->ordinateParam())));
  AL::Math::Interpolation::Key rightKey(rKey->value(),
                                        AL::Math::Interpolation::Tangent(static_cast<AL::Math::Interpolation::InterpolationType>(rKey->leftTangent()->interpType()),
                                                                         AL::Math::Position2D(rKey->leftTangent()->abscissaParam(),
                                                                                              rKey->leftTangent()->ordinateParam())),
                                        AL::Math::Interpolation::Tangent(static_cast<AL::Math::Interpolation::InterpolationType>(rKey->rightTangent()->interpType()),
                                                                         AL::Math::Position2D(rKey->rightTangent()->abscissaParam(),
                                                                                              rKey->rightTangent()->ordinateParam())));

  int keyIntervalSize = indexRightKey - indexLeftKey + 1;

  if (keyIntervalSize > 1)
  {
    // FIXME : minValue, maxValue
    float minValue = -std::numeric_limits<float>::max();
    float maxValue = std::numeric_limits<float>::max();

    std::vector<float> actuatorValues = interpolator.interpolate(keyIntervalSize, leftKey, rightKey,
                                                                 minValue, maxValue, valueIncrementLimit, 1);

    return actuatorValues[indexKey - indexLeftKey];
  }
  else
  {
    return leftKey.fValue;
  }
}

float TimelinePrivate::getMotionValue(const ActuatorCurveModel& curve, float value)
{
  ActuatorCurveModel::UnitType curveUnit = curve.unit();
  float result = value;
  switch(curveUnit)
  {
  case ActuatorCurveModel::UnitType_Degree:
    result *= AL::Math::TO_RAD;
    break;

  case ActuatorCurveModel::UnitType_Percent:
    break;

  case ActuatorCurveModel::UnitType_Undefined:
  default:
    if(curve.actuator() != "LHand" && curve.actuator() != "RHand")
    {
      result *= AL::Math::TO_RAD;
      break;
    }
  }

  return result;
}

bool TimelinePrivate::updateBezierAutoTangents(const int& currentIndex, KeyModelPtr key, const int &leftIndex, CKeyModelPtr &lNeighbor, const int &rightIndex, CKeyModelPtr &rNeighbor)
{
  using AL::Math::Position2D;

  // :TODO: jvuarand 20100406: merge that function with the one in Choregraphe

  //Note JB Desmottes 19-05-09 : we now consider that whenever the method is
  //  called, tangent params will change, and thus we do not need to inform the
  //  caller whether they really changed or not. In some cases, this will lead to
  //  unecessary updates.
  //  Example : current key is a minimum of the curve, and neighbor value (not the
  //  index) has changed. In that case, params of current key do not change.

  if (key->leftTangent()->interpType()==TangentModel::InterpolationType_BezierAuto || key->rightTangent()->interpType()==TangentModel::InterpolationType_BezierAuto)
  {
    float alpha = 1.0f/3.0f;
    float beta = 0.0f;

    if (lNeighbor && rNeighbor)
    {
      float value = key->value();
      float lvalue = lNeighbor->value();
      float rvalue = rNeighbor->value();
      if ((value < rvalue || value < lvalue)
        && (value > rvalue || value > lvalue))
      {
        if (currentIndex>=0 && leftIndex>=0 && rightIndex>=0)
        {
          beta = (rvalue - lvalue) / (rightIndex-leftIndex);

          // anti overshooting
          float tgtHeight = alpha * (rightIndex - currentIndex) * beta;
          if (fabs(tgtHeight) > fabs(rvalue - value))
          {
            beta *= (rvalue - value) / tgtHeight;
          }
          tgtHeight = alpha * (currentIndex - leftIndex) * beta;
          if (fabs(tgtHeight) > fabs(value - lvalue))
          {
            beta *= (value - lvalue) / tgtHeight;
          }
        }
      }
    }

    // set parameters into model
    if (key->leftTangent()->interpType()==TangentModel::InterpolationType_BezierAuto)
    {
      Position2D offset = Position2D(-alpha, -alpha*beta) * float(currentIndex - leftIndex);
      // :NOTE: for test purposes, you can force serialization of BEZIER_AUTO in Choregraphe, and enable assert below
      //assert(AL::Math::distanceSquared(key->fLeftTangent.fOffset, offset) < 0.01f);
      //key->fLeftTangent.fOffset = offset;

      key->leftTangent()->setAbscissaParam(offset.x);
      key->leftTangent()->setOrdinateParam(offset.y);
    }
    if (key->rightTangent()->interpType()==TangentModel::InterpolationType_BezierAuto)
    {
      Position2D offset = Position2D(alpha, alpha*beta) * float(rightIndex - currentIndex);
      // :NOTE: for test purposes, you can force serialization of BEZIER_AUTO in Choregraphe, and enable assert below
      //assert(AL::Math::distanceSquared(key->fRightTangent.fOffset, offset) < 0.01f);
      //key->fRightTangent.fOffset = offset;

      key->rightTangent()->setAbscissaParam(offset.x);
      key->rightTangent()->setOrdinateParam(offset.y);
    }

    return true;
  }
  else
    return false;
}

void TimelinePrivate::rebuildBezierAutoTangents(ActuatorCurveModelPtr curve)
{

  std::map<int, KeyModelPtr> keys = curve->keys();
  for (std::map<int, KeyModelPtr>::iterator it=keys.begin(), itEnd=keys.end(); it!=itEnd; ++it)
  {
    if (it->second->leftTangent()->interpType()==TangentModel::InterpolationType_BezierAuto
      || it->second->rightTangent()->interpType()==TangentModel::InterpolationType_BezierAuto)
    {
      int currentIndex = it->first;
      KeyModelPtr key = it->second;
      // get left neighbor, if any
      int leftIndex = currentIndex;
      CKeyModelPtr lNeighbor = CKeyModelPtr();
      if (it!=keys.begin())
      {
        std::map<int, KeyModelPtr>::iterator left = it;
        --left;
        leftIndex = left->first;
        lNeighbor = left->second;
      }
      // get right neighbor, if any
      int rightIndex = currentIndex;
      CKeyModelPtr rNeighbor = CKeyModelPtr();
      std::map<int, KeyModelPtr>::iterator right = it;
      ++right;
      if (right!=keys.end())
      {
        rightIndex = right->first;
        rNeighbor  = right->second;
      }
      // adjust this key
      updateBezierAutoTangents(currentIndex, key, leftIndex, lNeighbor, rightIndex, rNeighbor);
    }
  }
}

/* -- Public -- */
Timeline::Timeline(ObjectPtr memory, ObjectPtr motion)
  : _p (new TimelinePrivate(memory, motion))
{
}

Timeline::~Timeline()
{
  _p->stop();
  delete _p;
}

void Timeline::play(void)
{
  _p->play();
}

void Timeline::pause(void)
{
  _p->pause();
}

void Timeline::stop(void)
{
  _p->stop();
}

void Timeline::goTo(const int &pFrame)
{
  _p->goTo(pFrame);
}

int Timeline::getSize() const
{
  return _p->getSize();
}

int Timeline::getFPS() const
{
  return _p->getFPS();
}

void Timeline::setFPS(const int fps)
{
  _p->setFPS(fps);
}

void Timeline::setAnimation(AnimationModelPtr anim)
{
  _p->setAnimation(anim);
}

void Timeline::waitForTimelineCompletion()
{
  _p->_executer->waitForExecuterCompletion();
}

}

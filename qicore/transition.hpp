/**
* @author Aldebaran Robotics
* Aldebaran Robotics (c) 2012 All Rights Reserved
*/

#pragma once

#ifndef TRANSITION_H_
# define TRANSITION_H_

# include <string>

# include <qicore/api.hpp>

namespace qi
{

class TransitionPrivate;
class StateMachine;
class State;

class QICORE_API Transition
{
  public:
    Transition(State* to);
    ~Transition();

    void setName(std::string name);
    std::string getName() const;


    void trigger();

    State* getFromState() const;
    State* getToState() const;

    bool hasTimeOut() const;
    int getTimeOut() const;
    void setTimeOut(int n);

    TransitionPrivate* _p;
};

};

#endif /* !TRANSITION_H_ */
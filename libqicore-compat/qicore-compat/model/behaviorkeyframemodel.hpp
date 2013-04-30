/**
* @author Aldebaran Robotics
* Aldebaran Robotics (c) 2012 All Rights Reserved
*/

#pragma once

#ifndef BEHAVIORKEYFRAME_H_
#define BEHAVIORKEYFRAME_H_

#include <qicore-compat/api.hpp>
#include <alserial/alserial.h>

namespace qi {

  class BehaviorKeyFrameModelPrivate;
  class FlowDiagramModel;

  class QICORECOMPAT_API BehaviorKeyFrameModel
  {
  public:
    BehaviorKeyFrameModel(const std::string &name, int index, const std::string &bitmap, const std::string &path);
    BehaviorKeyFrameModel(boost::shared_ptr<const AL::XmlElement> elt, const std::string &dir);
    virtual ~BehaviorKeyFrameModel();

    const std::string& getName() const;
    int getIndex() const;
    const std::string& getBitmap() const;
    std::string getPath() const;

    void setName(const std::string& name);
    void setIndex(int index);
    void setBitmap(const std::string& bitmap);
    void setPath(const std::string& path);

  private:
    QI_DISALLOW_COPY_AND_ASSIGN(BehaviorKeyFrameModel);
    BehaviorKeyFrameModelPrivate *_p;
  };
  typedef boost::shared_ptr<BehaviorKeyFrameModel> BehaviorKeyFrameModelPtr;
}

#endif /* !BEHAVIORKEYFRAME_H_ */

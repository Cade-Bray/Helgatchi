#pragma once
#include <vector>
#include "IApp.h"

class AppRegistry {
public:
  void initAll(const AppContext& ctx);
  void broadcast(const Event& e, AppState& state, EventBus& bus);

private:
  std::vector<IApp*> apps_;
};

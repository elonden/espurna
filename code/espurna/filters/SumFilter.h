// -----------------------------------------------------------------------------
// Sum Filter
// Copyright (C) 2017-2019 by Xose Pérez <xose dot perez at gmail dot com>
// -----------------------------------------------------------------------------

#pragma once

#include "BaseFilter.h"

class SumFilter : public BaseFilter {
public:
    void update(double value) override {
        _value += value;
    }

    bool status() const override {
        return true;
    }

    void resize(size_t) override {
        _reset();
    }

    void reset() override {
        _reset();
    }

    double value() const override {
        return _value;
    }

private:
    void _reset() {
        _value = 0.0;
    }

    double _value = 0.0;
};

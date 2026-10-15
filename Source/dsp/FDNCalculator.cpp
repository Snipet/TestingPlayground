#include "FDNCalculator.h"

FDNConfig::FDNConfig(){}

void FDNConfig::calculate(float sample_rate, float room_size, float ms_decay){
    this->sample_rate = sample_rate;
    this->room_size = room_size;

    // Time for reverb tail to reach -60 db
    this->ms_decay = ms_decay;
}
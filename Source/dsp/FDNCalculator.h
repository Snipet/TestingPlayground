#include <stdlib.h>
#include <vector>

class FDNConfig {
public:
    FDNConfig();
    ~FDNConfig() = default;

    void calculate(float sample_rate, float room_size, float ms_decay);

private:
    size_t matrix_order;
    std::vector<size_t> delayline_lengths;

    float room_size;
    float sample_rate;
    float differential_decay;
    float ms_decay;

};
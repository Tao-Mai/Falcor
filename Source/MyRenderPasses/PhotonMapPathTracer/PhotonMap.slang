static const uint kPhotonCellCapacity = PHOTON_CELL_CAPACITY;

struct Photon
{
    float3 position;
    float3 normal;
    float3 directionToLight;
    float3 flux;
};

struct PhotonGrid
{
    float3 origin;
    float radius;
    uint3 dimensions;

    uint3 cell(float3 position)
    {
        float3 coordinate = (position - origin) / (2.f * radius);
        return uint3(clamp(coordinate, float3(0.f), float3(dimensions - 1u)));
    }

    uint index(uint3 cell)
    {
        return cell.x + dimensions.x * (cell.y + dimensions.y * cell.z);
    }
};

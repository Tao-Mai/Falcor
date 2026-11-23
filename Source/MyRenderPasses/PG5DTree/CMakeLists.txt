add_plugin(PG5DTree)

target_sources(PG5DTree PRIVATE
    PG5DTree.cpp
    PG5DTree.h
    PG5DTree.rt.slang
    PG5DTreeCache.slang
    UpdateRadianceCache.cs.slang
)

target_copy_shaders(PG5DTree MyRenderPasses/PG5DTree)

target_source_group(PG5DTree "MyRenderPasses")

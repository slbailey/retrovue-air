## 🧱 C++ Project Structure Standard

All RetroVue native modules (e.g. `retrovue-playout`, `retrovue-ingest`) must follow this layout:

```
project-root/
├─ include/
│  └─ retrovue/
│     └─ <module>/
│        └─ PublicHeader.h
├─ src/
│  └─ <module>/
│     └─ PrivateImplementation.cpp
├─ tests/
│  └─ ...
├─ CMakeLists.txt
```

### Rules

1. **Public headers go in `include/retrovue/<module>/`**  
   These define the module's external API and are imported by other RetroVue modules or bindings.  
   Example:

   ```cpp
   #include "retrovue/buffer/FrameRingBuffer.h"
   ```

2. **Private headers stay beside their .cpp files in `src/`**  
   Used for internal helpers or classes not exposed externally.

3. **CMake must export the include path:**

   ```cmake
   target_include_directories(${PROJECT_NAME}
       PUBLIC ${PROJECT_SOURCE_DIR}/include
   )
   ```

4. **Namespaces mirror directory hierarchy**

   ```cpp
   namespace retrovue::buffer {
       class FrameRingBuffer { ... };
   }
   ```

5. **No flat headers under `include/` — all public files must live under `retrovue/<module>/`.**

6. **Header/Source pairing**
   - `.h` → `include/retrovue/<module>/`
   - `.cpp` → `src/<module>/`

This ensures consistent structure across all RetroVue components, simplifies include paths, and supports clean separation between public and private APIs.

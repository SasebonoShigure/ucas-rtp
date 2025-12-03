[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/rummPfTQ)
# 2024-Lab2-RTP-Template

- 启动镜像：
  `docker run -it --rm --name netlab --mount "type=bind,source=path/to/code/in/host/machine networking,target=/home/student/workspace/code" --workdir /home/student/workspace/code -e TERM=xterm-256color -p 20729:22 -t code.lcpu.dev/2100013211/ns3:3.38`
- 启动第二个终端：
  `docker exec -it netlab bash -c "cd /home/student/workspace/code/ && bash"`

  Tutorial2-code 2来自助教

- usage:
  1. 在build目录`cmake .. -G "Unix Makefiles"`
  2. 如果需要开启`-DLDEBUG`，执行`cmake .. -DDL-ON`
  3. 编译，执行`make`
  4. 测试，`./rtp_test_all`
  5. 测试某个测试点，`./rtp_test_all --gtest_filter=RTP.XXX`
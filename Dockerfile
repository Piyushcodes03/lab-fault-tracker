FROM gcc:13
WORKDIR /app
COPY . .
RUN wget -q -O httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h
RUN g++ -std=c++17 backend.cpp -o backend -pthread
CMD ["./backend"]

module \top
  wire input 1 \I
  wire output 2 \O
  cell $pmux \m
    parameter \WIDTH 1
    parameter \S_WIDTH 2
    connect \A 1'0
    connect \B { \I 1'0 }
    connect \S { \I 1'0 }
    connect \Y \O
  end
end

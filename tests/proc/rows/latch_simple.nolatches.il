attribute \top 1
module \t
  wire input 1 \en
  wire width 4 input 2 \d
  wire width 4 output 3 \q
  attribute \full_case 1
  cell $mux $c7
    parameter \WIDTH 4
    connect \A 4'xxxx
    connect \B \d
    connect \S \en
    connect \Y \q
  end
end

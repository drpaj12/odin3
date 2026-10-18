attribute \top 1
module \m
  wire input 1 \en
  wire width 4 input 2 \d
  wire width 4 output 3 \q
  cell $dlatch $c16
    parameter \WIDTH 4
    parameter \EN_POLARITY 0
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end

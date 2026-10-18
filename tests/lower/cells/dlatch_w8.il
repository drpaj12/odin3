attribute \top 1
module \m
  wire input 1 \en
  wire width 8 input 2 \d
  wire width 8 output 3 \q
  cell $dlatch $c16
    parameter \WIDTH 8
    parameter \EN_POLARITY 1
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
